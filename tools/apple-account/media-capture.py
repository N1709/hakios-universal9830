#!/usr/bin/env python3
"""Coordinate one private response capture. Never print request/response bodies."""
import argparse
import io
import json
import os
from pathlib import Path
import plistlib
import re
import secrets
import shlex
import subprocess
import tarfile
import time

ROOT='/var/mobile/Library/RedwoodAppleAccountLab'

def response_summary(data):
    result={'bytes':len(data),'format':'binary','numeric_status':{}}
    try:
        obj=plistlib.loads(data);result['format']='plist'
    except Exception:
        try:obj=json.loads(data);result['format']='json'
        except Exception:
            if data.lstrip().lower().startswith((b'<!doctype html',b'<html')):result['format']='html'
            return result
    if not isinstance(obj,dict):return result
    paths=[(),('Header',),('Response',),('Response','Status'),('Status',),('error',)]
    categories=set()
    for path in paths:
        node=obj
        for key in path:node=node.get(key) if isinstance(node,dict) else None
        if not isinstance(node,dict):continue
        for key in ('ec','status','statusCode','errorCode','error-code','failureType','code'):
            value=node.get(key)
            if type(value) is int or isinstance(value,str) and re.fullmatch(r'-?\d{1,12}',value):
                result['numeric_status']['.'.join(path+(key,))]=int(value)
        for key in ('em','errorMessage','message','customerMessage'):
            text=node.get(key)
            if not isinstance(text,str) or len(text)>4096:continue
            text=text.lower()
            if 'password' in text and ('incorrect' in text or 'invalid' in text):categories.add('credentials_rejected')
            if 'device' in text and ('unsupported' in text or 'not supported' in text):categories.add('unsupported_device')
            if 'locked' in text or 'disabled' in text:categories.add('account_restricted')
            if 'review' in text or 'never been used' in text:categories.add('account_review')
            if 'anisette' in text:categories.add('machine_proof_related')
    result['categories']=sorted(categories)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['prepare','collect','disarm','watch'])
    parser.add_argument('--ssh-config',type=Path,required=True)
    parser.add_argument('--state',type=Path,default=Path('out/apple-account/private/media-capture-active.json'))
    parser.add_argument('--seconds',type=int,default=600)
    args=parser.parse_args()
    os.umask(0o077)
    ssh=['ssh','-F',str(args.ssh_config.resolve()),'-o','BatchMode=yes','-o','ConnectTimeout=4','s20']
    def remote(command,**kwargs):return subprocess.check_output(ssh+[command],timeout=45,**kwargs)
    def disarm():remote('rm -f '+ROOT+'/capture.json')
    if args.action=='prepare':
        if not 30<=args.seconds<=850:parser.error('Duration must be30..850 seconds')
        state={'run':secrets.token_hex(16),'began':time.time(),'expires_at':time.time()+args.seconds}
        directory=ROOT+'/private-capture/'+state['run']
        remote('umask 077; mkdir -p '+ROOT+'/private-capture; chmod 700 '+ROOT+'/private-capture; chown 501:501 '+ROOT+'/private-capture; mkdir -p '+shlex.quote(directory)+'; chmod 700 '+shlex.quote(directory)+'; chown 501:501 '+shlex.quote(directory))
        config={'enabled':True,'run':state['run'],'expires_at':state['expires_at']}
        subprocess.run(ssh+['umask 077; cat > '+ROOT+'/capture.json; chmod 600 '+ROOT+'/capture.json; chown 501:501 '+ROOT+'/capture.json'],input=json.dumps(config).encode(),check=True,timeout=15)
        args.state.parent.mkdir(parents=True,exist_ok=True);args.state.write_text(json.dumps(state,indent=2));args.state.chmod(0o600)
        print(json.dumps({'event':'capture_ready','run':state['run'],'duration_seconds':args.seconds,'request_bodies_recorded':False,'response_files_private':True}),flush=True)
        return
    state=json.loads(args.state.read_text())
    if not re.fullmatch(r'[a-f0-9]{32}',state.get('run','')):raise ValueError('Invalid run')
    directory=ROOT+'/private-capture/'+state['run']
    def collect():
        disarm()
        archive=remote('tar -cf - -C '+shlex.quote(directory)+' .')
        local=args.state.parent/('capture-'+state['run']);local.mkdir(mode=0o700,exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            for member in tar:
                name=member.name.removeprefix('./')
                if name in ('','.') and member.isdir():continue
                if not member.isfile() or not re.fullmatch(r'\d+(?:-\d+\.response|\.events\.jsonl)',name) or member.size>3*1024*1024:
                    raise ValueError('Unexpected private capture member')
                data=tar.extractfile(member).read();path=local/name;path.write_bytes(data);path.chmod(0o600)
        events=[]
        for path in local.glob('*.events.jsonl'):
            for line in path.read_text().splitlines():events.append(json.loads(line))
        rows=[]
        for start in (e for e in events if e['event']=='request_started'):
            end=next((e for e in events if e['event']=='request_finished' and e['transaction']==start['transaction']),{})
            body=local/(start['transaction']+'.response')
            row={k:start[k] for k in ('transaction','process','host','path','time')}
            row.update({k:end[k] for k in ('http_status','received_bytes','captured_bytes','truncated','elapsed_seconds','error_domain','error_code') if k in end})
            if body.exists():row['response_summary']=response_summary(body.read_bytes())
            rows.append(row)
        (local/'summary.json').write_text(json.dumps(rows,indent=2))
        for row in rows:print(json.dumps(row),flush=True)
        print(json.dumps({'event':'capture_collected','private_directory':str(local),'transactions':len(rows),'raw_values_printed':False}),flush=True)
    if args.action=='disarm':disarm();print('Capture disarmed; native services unchanged.');return
    if args.action=='collect':collect();return
    seen=set();terminal=None
    print('{"event":"capture_monitor_started"}',flush=True)
    while time.time()<state['expires_at']:
        command='find '+shlex.quote(directory)+" -maxdepth 1 -name '*.events.jsonl' -type f -exec cat {} \\; ; tail -100 "+ROOT+'/media-error-events.log'
        try:output=remote(command).decode()
        except (subprocess.SubprocessError,OSError):print('{"event":"capture_connection_lost_retrying"}',flush=True);time.sleep(3);continue
        for line in output.splitlines():
            try:event=json.loads(line)
            except ValueError:continue
            if event.get('time',0)<state['began']:continue
            kind=event.get('event')
            if kind not in ('request_started','request_finished','media_auth_flow'):continue
            identity=json.dumps(event,sort_keys=True)
            if identity in seen:continue
            seen.add(identity);print(json.dumps(event),flush=True)
            if kind=='media_auth_flow' and event.get('stage')=='finish_verified_account':terminal=terminal or time.time()
        if terminal and time.time()-terminal>3:break
        time.sleep(3)
    collect()

if __name__=='__main__':main()
