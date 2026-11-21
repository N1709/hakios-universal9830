#!/usr/bin/env python3
"""Install/rollback one immutable native account layer; preserve unrelated rules."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

PROCESSES = ['akd','accountsd','appleaccountd','amsaccountsd','appstored','itunesstored','storeaccountd','Preferences','AppStore']
OLD = re.compile(r'^/var/tmp/(?:anisette-lab-bridge-v[0-9]+|icloud-public-session-v[0-9]+|icloud-delegate-session-v[0-9]+)\.dylib(?:\s|$)')

def plan_rules(current, path):
    rows = [x.strip() for x in current.split(';') if x.strip()]
    removed = []
    for row in rows:
        if OLD.match(row):
            if not set(row.split()[1:]) <= set(PROCESSES):
                raise ValueError('Unexpected process in old account rule')
            removed.append(row)
    if not removed:
        raise ValueError('No known account rules to replace; inspect current state')
    rule = path+' '+' '.join(PROCESSES)
    if any('libredwood-account-layer-' in row for row in rows):
        raise ValueError('A shared layer is already configured; use status/rollback first')
    return ';'.join([x for x in rows if x not in removed]+[rule]), removed, rule

def rollback_rules(current, state):
    rows = [x.strip() for x in current.split(';') if x.strip()]
    if rows.count(state['installed_rule']) != 1:
        raise ValueError('Installed rule changed/missing; inspect before rollback')
    rows.remove(state['installed_rule'])
    for row in state['removed_rules']:
        if row not in rows:rows.append(row)
    return ';'.join(rows)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['plan','install','upgrade','rollback','status'])
    p.add_argument('--artifact',type=Path)
    p.add_argument('--ssh-config',type=Path)
    p.add_argument('--state',type=Path,default=Path('out/apple-account/private/shared-layer-active.json'))
    args=p.parse_args()
    ssh=['ssh']+(['-F',str(args.ssh_config.resolve())] if args.ssh_config else [])+['-o','BatchMode=yes','-o','ConnectTimeout=5','s20']
    def remote(command):return subprocess.check_output(ssh+[command],text=True)
    current=remote('/redwood/bin/rwsys kern.exec_inject').strip()
    if args.action=='status':
        print(json.dumps({'shared_rules':[r for r in current.split(';') if 'libredwood-account-layer-' in r],'state_present':args.state.exists()}));return
    if args.action=='rollback':
        state=json.loads(args.state.read_text());updated=rollback_rules(current,state)
    else:
        if args.artifact is None or not args.artifact.is_file():p.error('Need built --artifact')
        if not re.fullmatch(r'libredwood-account-layer-[0-9a-f]{16}\.dylib',args.artifact.name):p.error('Need immutable hashed artifact name')
        path='/var/jb/usr/lib/'+args.artifact.name
        if args.action=='upgrade':
            previous=json.loads(args.state.read_text())
            rows=[x.strip() for x in current.split(';') if x.strip()]
            if rows.count(previous['installed_rule'])!=1:raise ValueError('Shared rule changed; inspect before upgrade')
            rule=path+' '+' '.join(PROCESSES);removed=previous['removed_rules']
            updated=';'.join(rule if x==previous['installed_rule'] else x for x in rows)
        else:
            updated,removed,rule=plan_rules(current,path)
        state={'version':1,'created_at':time.time(),'installed_rule':rule,'removed_rules':removed,'artifact_sha256':hashlib.sha256(args.artifact.read_bytes()).hexdigest()}
        print(json.dumps({'action':args.action,'processes':PROCESSES,'replaced_account_rules':1 if args.action=='upgrade' else len(removed),'artifact':path}))
        if args.action=='plan':return
        if args.state.exists() and args.action!='upgrade':raise ValueError('State already exists; inspect before installing')
        remote('mkdir -p /var/jb/usr/lib')
        target='/var/tmp/'+args.artifact.name
        subprocess.run(['scp']+(['-F',str(args.ssh_config.resolve())] if args.ssh_config else [])+['-o','BatchMode=yes',str(args.artifact),'s20:'+target],check=True)
        remote('test ! -e '+shlex.quote(path)+' && cp '+shlex.quote(target)+' '+shlex.quote(path)+' && chmod 644 '+shlex.quote(path))
        data=subprocess.check_output(ssh+['cat '+shlex.quote(path)])
        if hashlib.sha256(data).hexdigest()!=state['artifact_sha256']:raise ValueError('Installed artifact readback mismatch')
        args.state.parent.mkdir(parents=True,exist_ok=True)
        if args.action=='upgrade':args.state.with_name(args.state.stem+'-before-upgrade-'+str(int(time.time()))+'.json').write_text(args.state.read_text())
        args.state.write_text(json.dumps(state,indent=2)+'\n');args.state.chmod(0o600)
    remote('/var/jb/usr/bin/killall -TERM Preferences AppStore 2>/dev/null || true')
    remote('/redwood/bin/rwsys kern.exec_inject '+shlex.quote(updated))
    if remote('/redwood/bin/rwsys kern.exec_inject').strip()!=updated:raise ValueError('Injection verification failed')
    restarted=[]
    for service in PROCESSES:
        if service in ('Preferences','AppStore'):continue
        for domain in ('user/501','system'):
            job=domain+'/com.apple.'+service
            exists=subprocess.run(ssh+['/var/jb/bin/launchctl print '+job],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
            if exists:
                subprocess.run(ssh+['/var/jb/bin/launchctl kickstart -k '+job],check=True,stdout=subprocess.DEVNULL)
                restarted.append(service);break
    if args.action=='rollback':args.state.rename(args.state.with_suffix('.rolled-back.json'))
    print(json.dumps({'configured':True,'restarted':restarted,'reboot_performed':False,'account_reset':False}))

if __name__=='__main__':main()
