import random, socket, errno, time, logging

from _coev import *
from _coev import __version__
"""
process-wide connection pool. 

should be something like sqlalchemy's QueuePool:
once a Client object is created and grabs connections to servers,
corresponding _Host instances are pinned to the coroutine that did it.
if and when Client releases the instances, they are marked as available.

all endpoints in one connection pool are considered equal, so .get()
does not take any arguments.


addresses are supplied as:
(AF, TYPE, (af-specific tuple))
(IPv4, Port), '.' in IPv4 is True, AF=AF_INET4, TYPE=SOCK_STREAM
(IPv6, Port), ':' in IPv6 is True, AF=AF_INET6, TYPE=SOCK_STREAM
(Path,), AF=AF_UNIX, TYPE=SOCK_STREAM


"""

class TooManyConnections(Exception):
    pass
    
class NoEndpointsConnectable(Exception):
    pass

class IOpTimeout(Exception):
    def __init__(self, connstr):
        self.msg = self.__class__.__name__ + ': ' + connstr
        
    def __str__(self):
        return self.msg

class ReadTimeout(IOpTimeout):
    pass
    
class WriteTimeout(IOpTimeout):
    pass
    


class Connection(object):
    """ those are stored in the connection pool """
    def __init__(self, pool, endpoint, conn_timeout, iop_timeout, read_limit):
        self.pool = pool
        
        s = socket.socket(endpoint[0], endpoint[1])
        s.setblocking(0)
        while True:
            try:
                s.connect(endpoint[2])
            except socket.error, msg:
                if msg[0] == errno.EINPROGRESS:
                    wait(s.fileno(), WRITE, conn_timeout)
                else:
                    s.close()
                    raise
            else:
                break
        self.sock = s
        self.sfile = socketfile(s.fileno(), 
            iop_timeout, read_limit)
        
        self.endpoint = endpoint
        self.dead = False
        
    def release(self):
        self.pool.release(self)
        
    def close(self):
        self.sock.close()
    
    def __str__(self):
        return self.__repr__()

    def __repr__(self):
        return "Connection(id={0:#08x} peer={1!r} endpoint={2!r})".format(id(self),self.sock.getpeername(), self.endpoint)
        
class ConnectionProxy(object):
    def __init__(self, connection):
        self.conn = connection

    def read(self, hint=0):
        try:
            return self.conn.sfile.read(hint)
        except Exception, e:
            self.conn.dead = True
            if e.errno == 110: 
                raise ReadTimeout(repr(self.conn))
            e.conn = repr(self.conn)
            raise e
        
    def readline(self, hint=0):
        try:
            return self.conn.sfile.readline(hint)
        except Exception, e:
            self.conn.dead = True
            if e.errno == 110: 
                raise ReadTimeout(repr(self.conn))
            e.conn = repr(self.conn)
            raise e
        
    def write(self, data):
        try:
            return self.conn.sfile.write(data)
        except Exception, e:
            self.conn.dead = True
            if e.errno == 110: 
                raise WriteTimeout(repr(self.conn))
            e.conn = repr(self.conn)
            raise e

    def __del__(self):
        self.conn.release()

class ConnectionPool(object):
    def __init__(self, conn_limit, conn_busy_wait, conn_timeout, iop_timeout, read_limit, *endpoints):
        self.el=logging.getLogger('coev.ConnectionPool')
        self.elstat=logging.getLogger('coev.ConnectionPool.stat')
        self.busy = []
        self.available = []
        self.conn_busy_wait = conn_busy_wait
        self.conn_limit = conn_limit
        self.conn_timeout = conn_timeout
        self.iop_timeout = iop_timeout
        self.read_limit = read_limit
        self.endpoints = []
        self.dead_endpoints = []
        self.gets = 0
        for ep in endpoints:
            if len(ep) == 1:
                self.endpoints.append((socket.AF_UNIX, socket.SOCK_STREAM, ep[0]))
            elif len(ep) == 2:
                if ':' in ep[0]:
                    self.endpoints.append((socket.AF_INET6, socket.SOCK_STREAM, ep))
                else:
                    self.endpoints.append((socket.AF_INET, socket.SOCK_STREAM, ep))
            elif len(ep) == 3:
                self.endpoints.append((ep[0], ep[1], ep[2]))
            else:
                raise ValueError("wrond endpoint format {0!r}".format(ep))
        
    def get(self):
        self.gets += 1
        if len(self.busy) == self.conn_limit: # implies that self.available is empty
            wait_start_time = time.time()
            while len(self.busy) == self.conn_limit:
                if time.time() - wait_start_time > self.conn_busy_wait:
                    raise TooManyConnections("to {0}; waited for {1} seconds".format(
                                  self.endpoints, time.time() - wait_start_time))
                sleep(self.conn_timeout)
        
        if len(self.available) > 0:
            conn = self.available.pop()
            self.busy.append(conn)
            self.elstat.debug("get(): [{4}] Avail {0} Busy {1} Gets {2} giving {3}".format(
                    len(self.available), len(self.busy), self.gets, id(conn), getpos()))
            return ConnectionProxy(conn)
        
        endpoints = list(self.endpoints)
        endpoints.sort(cmp = lambda a,b: random.randint(-1,1))
        conn = None
        failstr = ''
        faillist = []
        #self.el.debug('%r', endpoints) 
        for endpoint in endpoints:
            try:
                failstr = None
                conn = Connection(self, endpoint, self.conn_timeout, self.iop_timeout, self.read_limit)
            except Timeout:
                failstr = '{0}: timeout'.format(endpoint)
            except socket.error, e:
                failstr = '{0}: socket.error: {1} ({2})'.format(endpoint, e.strerror, e.errno)
            else:
                self.el.info('new connection to %s', endpoint)
                break
            if failstr:
                self.el.error(failstr)
                faillist.append(failstr)
        if not conn:
            raise NoEndpointsConnectable(';'.join(faillist))
            
        self.busy.append(conn)
        self.elstat.debug("[{4}] Avail {0} Busy {1} Gets {2} giving new {3}".format(
                len(self.available), len(self.busy), self.gets, id(conn), getpos()))
        return ConnectionProxy(conn)
        
    def release(self, conn):
        self.busy.remove(conn)
        if conn.dead is not True:
            self.available.append(conn)
            self.elstat.debug("release():[{4}] Avail {0} Busy {1} Gets {2} returned {3}".format(
                    len(self.available), len(self.busy), self.gets, id(conn), getpos()))

    def drop_idle(self):
        for c in self.available:
            c.close()
        self.available = []

# simple connect

def test_one(addr):
    cp = ConnectionPool(10, 5.0, 2.0, 5.0, 4096, addr)
    
    def foo():
        conn = cp.get()
        print "conn", repr(conn), "busy", repr(cp.busy), "available", repr(cp.available)
    foo()
    print "busy", repr(cp.busy), "available", repr(cp.available)
    print cp.available[0].endpoint
    import time
    time.sleep(150)
    
    
def _do_stuff():
    import sys
    addr = ( '127.0.0.1', 22 )
    if len(sys.argv) == 3:
        try:
            addr = ( sys.argv[1], int(sys.argv[2] ))
        except:
            pass
    print "addr is {0}".format(addr)
    test_one(addr)
        

if __name__ == "__main__":
    import thread
    thread.start_new_thread(_do_stuff, ())
    scheduler()
    
    
    
    
    
    
    
    
