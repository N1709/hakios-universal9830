from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
from pathlib import Path
import threading,subprocess,time,json,select,os
state={'error':'Taking first reading'}
root=Path(__file__).parent
log=open(root/'readings.jsonl','a',buffering=1)
def collect():
 global state
 while True:
  proc=None
  try:
   proc=subprocess.Popen(['ssh','-o','ConnectTimeout=3','-o','ServerAliveInterval=2','-o','ServerAliveCountMax=1','s20','/var/tmp/codex-als-live-readings --stream'],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
   pending=b''
   while proc.poll() is None:
    if not select.select([proc.stdout],[],[],5)[0]:raise RuntimeError('No fresh readings from phone')
    chunk=os.read(proc.stdout.fileno(),8192)
    if not chunk:break
    pending+=chunk
    if len(pending)>65536:raise RuntimeError('Invalid phone output')
    while b'\n' in pending:
     line,pending=pending.split(b'\n',1)
     v=json.loads(line);v['updated']=time.time();state=v;log.write(json.dumps(v)+'\n')
   raise RuntimeError('Phone disconnected; reconnecting')
  except Exception as e:state={'error':str(e),'updated':time.time()}
  finally:
   if proc:
    proc.terminate()
    try:proc.wait(timeout=3)
    except subprocess.TimeoutExpired:proc.kill();proc.wait()
  time.sleep(1)
class Handler(BaseHTTPRequestHandler):
 def do_GET(self):
  if self.path=='/api/readings':body=json.dumps(state).encode();typ='application/json'
  elif self.path=='/':body=(root/'index.html').read_bytes();typ='text/html; charset=utf-8'
  else:self.send_error(404);return
  self.send_response(200);self.send_header('Content-Type',typ);self.send_header('Cache-Control','no-store');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
 def log_message(self,*args):pass
threading.Thread(target=collect,daemon=True).start()
print('Live dashboard: http://127.0.0.1:8765',flush=True)
ThreadingHTTPServer(('127.0.0.1',8765),Handler).serve_forever()
