#!/usr/bin/env python3
"""Owner-authorized one-shot phone-PET -> native Mac comparison.
Separate from the machine-proof broker. No request/token files or raw logging.
"""
import argparse
import base64
import urllib.request
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def valid_request(value):
    if not isinstance(value, dict) or set(value) != {'version','nonce','issued_at','username','pet','dsid','alt_dsid'}:
        return False
    if type(value['version']) is not int or value['version'] != 1 or type(value['issued_at']) not in (int,float) or abs(value['issued_at']-time.time())>60:
        return False
    for name,limit in [('username',320),('pet',16384),('dsid',20),('alt_dsid',256),('nonce',64)]:
        text=value[name]
        if not isinstance(text,str) or not 0<len(text)<=limit or any(ord(c)<32 or ord(c)==127 for c in text):return False
    return bool('@' in value['username'] and re.fullmatch(r'[0-9]{1,20}',value['dsid']) and re.fullmatch(r'[A-Za-z0-9_-]{16,64}',value['nonce']))


def selftest():
    value={'version':1,'nonce':'synthetic_nonce_012345','issued_at':time.time(),'username':'fixture@example.invalid','pet':'synthetic-token','dsid':'123','alt_dsid':'synthetic-alt'}
    assert valid_request(value)
    assert not valid_request(dict(value,password='not-accepted'))
    assert not valid_request(dict(value,issued_at=0))
    assert not valid_request(dict(value,pet='line\nbreak'))
    assert not valid_request(dict(value,dsid='not-numeric'))
    print('PASS: comparison accepts bounded fresh PET envelope; raw-password/extra fields, stale and malformed requests refused')


def safe_application_summary(value):
    if not isinstance(value,dict):return {}
    def node(item):
        if not isinstance(item,dict):return {}
        out={k:v for k,v in item.items() if k in ('status','status-code','statusCode','errorCode','error-code') and type(v) is int and abs(v)<10**12}
        for k in ('error_field_present','tokens_field_present'):
            if type(item.get(k)) is bool:out[k]=item[k]
        return out
    out={k:value[k] for k in ('parsed_dictionary','account_info_present') if type(value.get(k)) is bool}
    out['root']=node(value.get('root'))
    services=value.get('delegates',{})
    out['delegates']={k:node(services[k]) for k in ('com.apple.mobileme','com.apple.gamecenter','com.apple.private.ids') if isinstance(services,dict) and k in services}
    return out


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test',action='store_true')
    p.add_argument('--key',type=Path);p.add_argument('--known-hosts',type=Path);p.add_argument('--token-file',type=Path)
    p.add_argument('--wait-for-phone-failure',action='store_true')
    p.add_argument('--phone-worker',action='store_true')
    p.add_argument('--phone-body-token-file',type=Path)
    p.add_argument('--worker',default='native-pet-comparison-v4')
    p.add_argument('--host');p.add_argument('--port',type=int,default=49751)
    args=p.parse_args()
    if args.self_test:selftest();return
    if not args.host or not re.fullmatch(r'[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+',args.host) or not all([args.key,args.known_hosts,args.token_file]):p.error('Need pinned SSH options/token')
    if not re.fullmatch(r'native-pet-comparison-v[0-9]+',args.worker):p.error('Invalid worker name')
    token=args.token_file.read_text().strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}',token):raise ValueError('Invalid one-shot bearer')
    used=False;lock=threading.Lock();failure=threading.Condition();last_failure=[0.0];watcher=None
    watch_stop=threading.Event()
    if args.wait_for_phone_failure:
        def watch():
            nonlocal watcher
            while not watch_stop.is_set():
                watcher=subprocess.Popen(['ssh','-o','BatchMode=yes','-o','ConnectTimeout=4','-o','ServerAliveInterval=5','-o','ServerAliveCountMax=2','s20','tail -n 0 -F /var/mobile/Library/RedwoodAppleAccountLab/network-events.log'],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
                for line in watcher.stdout:
                    try:row=json.loads(line)
                    except (ValueError,UnicodeError):continue
                    if row.get('event')=='account_request_completion' and row.get('request_class')=='AALoginAccountRequest' and row.get('status')==401:
                        with failure:last_failure[0]=time.time();failure.notify_all()
                watcher.wait()
                watch_stop.wait(2)
        threading.Thread(target=watch,daemon=True).start()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*unused):pass
        def reply(self,status,value):
            body=json.dumps(value,separators=(',',':')).encode();self.send_response(status)
            self.send_header('Content-Type','application/json');self.send_header('Cache-Control','no-store');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        def do_POST(self):
            nonlocal used
            if self.path!='/compare-once':self.reply(404,{'error':'not_found'});return
            if not secrets.compare_digest(self.headers.get('Authorization',''),'Bearer '+token):self.reply(401,{'error':'unauthorized'});return
            try:
                size=int(self.headers.get('Content-Length','0'))
                if not 0<size<=32768:raise ValueError()
                value=json.loads(self.rfile.read(size))
                if not valid_request(value):raise ValueError()
            except Exception:self.reply(400,{'error':'invalid_request'});return
            with lock:
                if used:self.reply(409,{'error':'comparison_already_used'});return
                used=True
            command=['ssh','-i',str(args.key.resolve()),'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(args.known_hosts.resolve()),'-o','ConnectTimeout=5',args.host,'.local/share/redwood-account-lab/'+args.worker+' --compare-once 2>/dev/null']
            if args.phone_worker:
                command=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=4','s20','/var/jb/usr/bin/su mobile -c \"/var/tmp/'+args.worker+' --compare-once\"']
            try:
                if args.wait_for_phone_failure:
                    with failure:
                        if not failure.wait_for(lambda:last_failure[0]>=value['issued_at'],timeout=45):raise TimeoutError()
                if args.phone_body_token_file:
                    bearer=args.phone_body_token_file.read_text().strip()
                    req=urllib.request.Request('http://127.0.0.1:49741/v1/recent-signing-metadata',headers={'Authorization':'Bearer '+bearer})
                    with urllib.request.urlopen(req,timeout=4) as response:
                        metadata=json.loads(response.read(24000))
                    if not value['issued_at']-2 <= metadata['issued_at'] <= time.time() or time.time()-metadata['issued_at']>90:
                        raise ValueError('Stale signing metadata')
                    # Reuse the broker's narrow device/service schema; no credential fields.
                    spec=importlib.util.spec_from_file_location('native_broker_schema',Path(__file__).with_name('native-provider-server.py'))
                    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
                    body=base64.b64decode(metadata['icloud_body'],validate=True)
                    if not module.valid_signing_metadata(body):raise ValueError('Invalid signing metadata')
                    value['icloud_body']=metadata['icloud_body']
                    headers=metadata.get('machine_headers')
                    if not isinstance(headers,dict) or set(headers)!=module.FIELDS or any(not isinstance(v,str) or not 0<len(v)<=16384 or '\r' in v or '\n' in v for v in headers.values()):
                        raise ValueError('Invalid machine snapshot')
                    value['machine_headers']=headers
                process=subprocess.run(command,input=(json.dumps(value,separators=(',',':'))+'\n').encode(),stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=48)
                if len(process.stdout)>8192:raise ValueError()
                result=json.loads(process.stdout)
                if not isinstance(result,dict) or result.get('version')!=1 or result.get('nonce')!=value['nonce'] or result.get('account_saved') is not False:raise ValueError()
                status=result.get('http_status',0)
                if type(status) is not int or not 0<=status<=599:raise ValueError()
                errors=[]
                for item in result.get('errors',[])[:4]:
                    if isinstance(item,dict) and item.get('domain') in ('com.apple.appleaccount','com.apple.AppleAccount.Error','NSURLErrorDomain','NSCocoaErrorDomain','other') and type(item.get('code')) is int:errors.append({'domain':item['domain'],'code':item['code']})
                safe={'version':1,'nonce':value['nonce'],'http_status':status,'errors':errors,'response_present':bool(result.get('response_present')),'account_saved':False}
                summary=result.get('application_summary')
                if isinstance(summary,dict):
                    # Worker emits only fixed labels/codes. Never log raw responses.
                    print(json.dumps({'event':'native_application_summary','summary':safe_application_summary(summary)}),flush=True)
                stage=result.get('error')
                if stage in ('machine_metadata_unavailable','native_signing_unavailable','timeout'):safe['worker_failure_stage']=stage
                self.reply(200,safe)
                print(json.dumps({'event':'native_pet_comparison_finished','worker_target':'phone' if args.phone_worker else 'mac','http_status':status,'errors':errors,'worker_failure_stage':safe.get('worker_failure_stage')}),flush=True)
                threading.Thread(target=server.shutdown,daemon=True).start()
            except Exception:
                self.reply(502,{'error':'native_comparison_unavailable'})
                print('{"event":"native_pet_comparison_unavailable"}',flush=True)
                threading.Thread(target=server.shutdown,daemon=True).start()
    server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    print('{"event":"one_shot_comparison_ready"}',flush=True)
    deadline=threading.Timer(1800,server.shutdown);deadline.daemon=True;deadline.start()
    try:server.serve_forever()
    finally:
        watch_stop.set()
        deadline.cancel();server.server_close()
        if watcher is not None:
            watcher.terminate()
            try:watcher.wait(timeout=2)
            except subprocess.TimeoutExpired:watcher.kill();watcher.wait()

if __name__=='__main__':main()
