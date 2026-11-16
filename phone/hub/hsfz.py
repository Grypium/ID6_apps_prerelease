import socket, struct, time
class GW:
    def __init__(s, tester=0xF5):
        s.t=tester; s.s=socket.create_connection(("127.0.0.1",6801),timeout=5); s.s.settimeout(0.05); s.buf=b""
    def ask(s, target, uds, timeout=0.5):
        body=bytes([s.t,target])+uds; s.s.sendall(struct.pack(">IH",len(body),1)+body)
        end=time.time()+timeout; t0=time.time()
        while time.time()<end:
            try: s.buf+=s.s.recv(4096)
            except socket.timeout: pass
            while len(s.buf)>=6:
                n,c=struct.unpack(">IH",s.buf[:6])
                if len(s.buf)<6+n: break
                b,s.buf=s.buf[6:6+n],s.buf[6+n:]
                if c==0x12: s.s.sendall(struct.pack(">IH",2,0x13)+b"\x00"+bytes([s.t])); continue
                if c!=1 or len(b)<3 or b[0]!=target or b[1]!=s.t: 
                    if c not in (1,2): return ("ctrl",c,b.hex())
                    continue
                r=b[2:]
                if r[0]==0x7F and len(r)>2 and r[2]==0x78: end=time.time()+3; continue
                return (r, time.time()-t0)
        return None
