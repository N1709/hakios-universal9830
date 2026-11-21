"""Private native Mescal stdio transport; no input/proof/credential logging."""
import base64
import json
import os
import select
import subprocess
import threading
import time

OPERATIONS = {'sign', 'prime', 'verify', 'verify-prime', 'store-proof', 'store-device', 'build-store-request'}
STORE_ENDPOINTS = {
    'https://auth.itunes.apple.com/auth/v1/native', 'https://auth.itunes.apple.com/auth/v1/native/',
    'https://auth.itunes.apple.com/auth/v1/native/fast', 'https://auth.itunes.apple.com/auth/v1/native/fast/'}
def valid_request(value):
    if not isinstance(value, dict) or value.get('operation') not in OPERATIONS:
        return False
    expected={'version','nonce','operation','endpoint'} if value['operation']=='store-proof' else {'version','nonce','operation','data'}
    if value['operation']=='store-device':expected={'version','nonce','operation'}
    if value['operation']=='build-store-request':expected={'version','nonce','operation','endpoint','parameters'}
    if value['operation']=='verify':expected.add('signature')
    if set(value)!=expected or type(value['version']) is not int or value['version']!=1:
        return False
    import re
    if not isinstance(value['nonce'],str) or not re.fullmatch(r'[A-Za-z0-9_-]{16,64}',value['nonce']):return False
    if value['operation'] in ('store-proof','build-store-request') and value.get('endpoint') not in STORE_ENDPOINTS:return False
    if value['operation']=='build-store-request':
        p=value.get('parameters')
        if not isinstance(p,str) or len(p)>200000:return False
        try:
            if not 0<len(base64.b64decode(p,validate=True))<=131072:return False
        except (ValueError,TypeError):return False
    for field in ('data','signature'):
        if field not in value:continue
        try:
            if not isinstance(value[field],str) or len(value[field])>180000:return False
            data=base64.b64decode(value[field],validate=True)
            if not 0<len(data)<=131072:return False
        except (ValueError,TypeError):return False
    return True

class MediaTransport:
    def __init__(self, command):
        self.command=command
        self.process=None
        self.lock=threading.Lock()
    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=2)
    def fetch(self,value):
        if not valid_request(value):raise ValueError('Invalid media request')
        with self.lock:
            if self.process is None or self.process.poll() is not None:
                self.process=subprocess.Popen(self.command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
            try:
                self.process.stdin.write((json.dumps(value,separators=(',',':'))+'\n').encode())
                end=time.monotonic()+30;output=bytearray()
                while b'\n' not in output:
                    remaining=end-time.monotonic()
                    if remaining<=0 or not select.select([self.process.stdout],[],[],remaining)[0]:raise TimeoutError('Media helper timeout')
                    chunk=os.read(self.process.stdout.fileno(),65536)
                    if not chunk:raise RuntimeError('Media helper closed')
                    output.extend(chunk)
                    if len(output)>200000:raise ValueError('Oversized media reply')
                result=json.loads(output)
                if result.get('version')!=1 or result.get('nonce')!=value['nonce']:raise ValueError('Invalid media reply')
                if 'error' in result:
                    if result['error'] not in {'invalid_request','unsupported_interface','timeout','native_failure','invalid_native_result'}:raise ValueError('Unknown failure')
                    return {k:result[k] for k in ('version','nonce','error')}
                if value['operation']=='store-device':
                    import re
                    if not isinstance(result.get('device_guid'),str) or not re.fullmatch(r'[A-Fa-f0-9]{12}',result['device_guid']):raise ValueError('Invalid Store device identity')
                    return {'version':1,'nonce':value['nonce'],'device_guid':result['device_guid']}
                if value['operation']=='store-proof':
                    headers=result.get('headers')
                    if not isinstance(headers,dict) or set(headers)!={'X-Apple-AMD','X-Apple-AMD-M'} or any(not isinstance(v,str) or not 0<len(v)<=16384 or '\n' in v or '\r' in v for v in headers.values()):raise ValueError('Invalid Store proof')
                    return {'version':1,'nonce':value['nonce'],'headers':headers}
                if value['operation']=='build-store-request':
                    url=result.get('url');headers=result.get('request_headers');body=result.get('body')
                    if url not in STORE_ENDPOINTS:raise ValueError('Invalid built url')
                    if not isinstance(headers,dict) or not headers or len(headers)>40 or any(not isinstance(k,str) or not isinstance(v,str) or not 0<len(v)<=16384 or '\n' in v or '\r' in v for k,v in headers.items()):raise ValueError('Invalid built headers')
                    if 'X-Apple-AMD' not in headers or 'X-Apple-ActionSignature' not in headers:raise ValueError('Missing proof headers')
                    if not isinstance(body,str) or len(body)>200000:raise ValueError('Invalid built body')
                    try:
                        if len(base64.b64decode(body,validate=True))>131072:raise ValueError()
                    except (ValueError,TypeError):raise ValueError('Invalid built body')
                    return {'version':1,'nonce':value['nonce'],'url':url,'request_headers':headers,'body':body}
                if value['operation'].startswith('verify'):
                    if type(result.get('verified')) is not bool:raise ValueError('Invalid verification result')
                    return {'version':1,'nonce':value['nonce'],'verified':result['verified']}
                signature=result.get('signature')
                if not isinstance(signature,str) or not 0<len(signature)<=180000 or not 0<len(base64.b64decode(signature,validate=True))<=131072:raise ValueError('Invalid signature')
                return {'version':1,'nonce':value['nonce'],'signature':signature}
            except Exception:
                self.close()
                raise
