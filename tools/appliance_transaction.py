# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

"""Recoverable changes to the fixed headunit service-unit bundle; never restart."""
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile

LEGACY_MANAGED = tuple('headunit-'+name+'.service' for name in
                ('compositor','outputs','cluster','receiver','vehicle'))
CAMERA_MANAGED = LEGACY_MANAGED + ('headunit-camera.service',)
MANAGED = CAMERA_MANAGED + ('headunit-phone-page.service',)
MAX_UNIT = 65536
MAX_JOURNAL = 1024*1024
MAX_SNAPSHOTS = 64
STATES = ('prepared','applying','rollback_pending','complete','rolled_back')
ENABLE_STATES = ('enabled','disabled','static','not-found')


def _directory(path, private=False, create=False):
    path=Path(path)
    if not path.is_absolute() or str(path)!=os.path.normpath(str(path)):
        raise ValueError('Transaction paths must be exact absolute paths')
    if create and not path.exists():
        missing=[]; parent=path
        while not parent.exists():
            missing.append(parent); parent=parent.parent
        _directory(parent)
        for item in reversed(missing): item.mkdir(mode=0o700)
    info=path.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.getuid() or
            info.st_mode & (0o077 if private else 0o022) or path.resolve()!=path):
        raise ValueError('Transaction directory has an unexpected owner, permissions or path')
    for ancestor in path.parents:
        parent=ancestor.lstat()
        if (not stat.S_ISDIR(parent.st_mode) or parent.st_uid not in (0,os.getuid()) or
                (parent.st_mode&0o022 and not (parent.st_uid==0 and parent.st_mode&stat.S_ISVTX))):
            raise ValueError('Transaction directory has an untrusted or replaceable ancestor')
    return path


def _read(path, maximum, private=False):
    fd=os.open(path,os.O_RDONLY|os.O_CLOEXEC|os.O_NOFOLLOW|os.O_NONBLOCK)
    try:
        info=os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or
                info.st_mode & (0o077 if private else 0o022) or info.st_size>maximum):
            raise ValueError('Managed file has an unexpected owner, permissions, type or size')
        data=os.read(fd,maximum+1)
        if len(data)!=info.st_size: raise ValueError('Managed file changed while reading')
        return data,stat.S_IMODE(info.st_mode)
    finally: os.close(fd)


def _sync(directory):
    fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC)
    try: os.fsync(fd)
    finally: os.close(fd)


def _write(path,data,mode):
    fd,name=tempfile.mkstemp(prefix='.'+path.name+'-',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as out:
            os.fchmod(out.fileno(),mode); out.write(data); out.flush(); os.fsync(out.fileno())
        os.replace(name,path); _sync(path.parent)
    finally:
        if os.path.exists(name): os.unlink(name)


def _unique(pairs):
    result={}
    for key,value in pairs:
        if key in result: raise ValueError('Duplicate transaction journal field')
        result[key]=value
    return result


def _record(data=None,mode=0,enabled='not-found'):
    return {'exists':data is not None,'data':base64.b64encode(data).decode('ascii') if data is not None else '',
            'sha256':hashlib.sha256(data).hexdigest() if data is not None else '',
            'mode':mode,'enabled':enabled}


def _data(record):
    if (not isinstance(record,dict) or set(record)!={'exists','data','sha256','mode','enabled'} or
            type(record['exists']) is not bool or type(record['mode']) is not int or
            not 0<=record['mode']<=0o777 or record['mode']&0o022 or
            record['enabled'] not in ENABLE_STATES or not isinstance(record['data'],str) or
            len(record['data'])>((MAX_UNIT+2)//3)*4):
        raise ValueError('Invalid managed-unit journal record')
    data=base64.b64decode(record['data'],validate=True)
    expected=hashlib.sha256(data).hexdigest() if record['exists'] else ''
    if (len(data)>MAX_UNIT or record['sha256']!=expected or
            (not record['exists'] and (data or record['mode'] or record['enabled']!='not-found'))):
        raise ValueError('Invalid managed-unit journal payload')
    return data if record['exists'] else None


class Transaction:
    def __init__(self,unit_directory,state_directory,runner=subprocess.run):
        self.units=_directory(unit_directory)
        self.state=_directory(state_directory,True,True)
        self.runner=runner
        self.lock=None

    def __enter__(self):
        self.lock=os.open(self.state/'install.lock',os.O_RDWR|os.O_CREAT|os.O_CLOEXEC|os.O_NOFOLLOW|os.O_NONBLOCK,0o600)
        try:
            info=os.fstat(self.lock)
            if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or info.st_mode&0o077:
                raise ValueError('Unsafe appliance transaction lock')
            fcntl.flock(self.lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BaseException:
            os.close(self.lock); self.lock=None; raise
        return self

    def __exit__(self,*_):
        os.close(self.lock); self.lock=None

    def command(self,args,check=True):
        return self.runner(args,check=check,capture_output=True,text=True,timeout=15)

    def enabled(self,name):
        result=self.command(['systemctl','is-enabled',name],False)
        value=result.stdout.strip()
        if value not in ENABLE_STATES:
            raise ValueError('Unsupported managed-unit enable state; unmask/unlink it before installing: '+name)
        return value

    def current(self,name):
        try: data,mode=_read(self.units/name,MAX_UNIT)
        except FileNotFoundError: return _record()
        return _record(data,mode)

    def journal(self,folder):
        folder=Path(folder)
        if folder.parent!=self.state or not re.fullmatch(r'transaction-[a-z0-9_-]{8,64}',folder.name):
            raise ValueError('Rollback must select a snapshot inside the private transaction directory')
        _directory(folder,True)
        raw,_=_read(folder/'journal.json',MAX_JOURNAL,True)
        value=json.loads(raw,object_pairs_hook=_unique)
        if (not isinstance(value,dict) or type(value.get('version')) is not int or
                value['version'] not in (1,2) or
                set(value)!={'version','unit_directory','state','previous','next'} | ({'releases'} if value['version']==2 else set()) or
                value['unit_directory']!=str(self.units) or value['state'] not in STATES):
            raise ValueError('Invalid appliance transaction journal')
        scope=set(value['previous']) if isinstance(value['previous'],dict) else set()
        if scope not in (set(LEGACY_MANAGED),set(CAMERA_MANAGED),set(MANAGED)):
            raise ValueError('Journal contains an unmanaged or missing service name')
        for key in ('previous','next'):
            if not isinstance(value[key],dict) or set(value[key])!=scope:
                raise ValueError('Journal contains an unmanaged or missing service name')
            for record in value[key].values(): _data(record)
        if value['version']==2:
            from appliance_release import unit_binding
            if not isinstance(value['releases'],dict) or set(value['releases'])!={'previous','next'}:
                raise ValueError('Invalid application release journal bindings')
            for key in ('previous','next'):
                derived=unit_binding({name:_data(record).decode('utf-8') for name,record in value[key].items() if record['exists']},self.units)
                if value['releases'][key]!=derived:
                    raise ValueError('Journal release binding differs from its exact unit payload')
        return value

    def save(self,folder,journal,state):
        journal['state']=state
        raw=json.dumps(journal,separators=(',',':')).encode()
        if len(raw)>MAX_JOURNAL: raise ValueError('Appliance transaction journal is too large')
        _write(folder/'journal.json',raw,0o600)

    def set_unit(self,name,record):
        data=_data(record); path=self.units/name
        if data is None:
            try: path.unlink()
            except FileNotFoundError: pass
            _sync(self.units)
        else: _write(path,data,record['mode'])

    def guard_current(self,journal):
        for name in journal['previous']:
            current=self.current(name)
            fingerprint=lambda record:(record['exists'],record['data'],record['mode'])
            if fingerprint(current) not in (fingerprint(journal['previous'][name]),fingerprint(journal['next'][name])):
                raise ValueError('Managed unit changed outside this transaction; refusing to overwrite it: '+name)

    def rollback(self,folder):
        if self.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
        journal=self.journal(folder)
        if journal['state']=='rolled_back': return
        # Five- and six-unit journals own only their recorded names. Restoring
        # one must not change a later camera or resident phone-page service.
        managed=tuple(name for name in MANAGED if name in journal['previous'])
        self.guard_current(journal)
        if journal['version']==2 and journal['releases']['previous']:
            from appliance_release import verify_unit_bundle
            # A broken candidate must not block a good rollback. Verify the
            # target release, not the release being discarded.
            verify_unit_bundle(journal['releases']['previous'],
                {name:_data(record).decode('utf-8') for name,record in journal['previous'].items() if record['exists']},
                self.units)
        if journal['state']=='prepared':
            self.save(folder,journal,'rolled_back'); return
        self.save(folder,journal,'rollback_pending')
        # Disable while the current file still contains its Install section.
        # This never stops a running service, and cleans this bundle's boot links.
        for name in managed:
            if self.current(name)['exists']: self.command(['systemctl','disable',name])
        for name in managed: self.set_unit(name,journal['previous'][name])
        self.command(['systemctl','daemon-reload'])
        for name in managed:
            old=journal['previous'][name]
            if old['enabled']=='enabled': self.command(['systemctl','enable',name])
            elif old['exists']: self.command(['systemctl','disable',name])
        self.save(folder,journal,'rolled_back')
        # A V5 recovery owner can restore an older appliance's exact retained
        # bootstrap. Complete the existing unit journal first: a crash here is
        # repaired by boot recovery using the restored sealed unit binding.
        try:
            from appliance_boot import align_boot_with_units
        except ImportError:
            pass  # Retained pre-migration inventories have no alignment API.
        else:
            align_boot_with_units(self)

    def recover_pending(self):
        if self.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
        folders=[p for p in self.state.iterdir() if p.name.startswith('transaction-')]
        if len(folders)>MAX_SNAPSHOTS: raise ValueError('Too many retained appliance snapshots; archive older completed ones')
        for folder in sorted(folders):
            # A crash before the first durable journal cannot have changed units.
            if not (folder/'journal.json').exists():
                _directory(folder,True); continue
            journal=self.journal(folder)
            if journal['state'] in ('prepared','applying','rollback_pending'): self.rollback(folder)

    def install(self,units):
        if self.lock is None: raise RuntimeError('Acquire the appliance transaction lock first')
        if (not isinstance(units,dict) or not units or set(units)-set(MANAGED) or
                any(not isinstance(text,str) or '\0' in text or len(text.encode())>MAX_UNIT for text in units.values())):
            raise ValueError('Invalid or unmanaged appliance unit bundle')
        self.recover_pending()
        if sum(p.name.startswith('transaction-') for p in self.state.iterdir())>=MAX_SNAPSHOTS:
            raise ValueError('Snapshot limit reached; archive older completed appliance snapshots before installing')
        previous={}
        for name in MANAGED:
            record=self.current(name); record['enabled']=self.enabled(name)
            if not record['exists'] and record['enabled']!='not-found':
                raise ValueError('Managed unit is loaded outside the local unit directory; refusing to replace it: '+name)
            previous[name]=record
        desired={name:_record(units[name].encode(),0o644,'enabled') if name in units else _record() for name in MANAGED}
        from appliance_release import unit_binding, verify, verify_unit_bundle
        releases={'previous':unit_binding({name:_data(record).decode('utf-8') for name,record in previous.items() if record['exists']},self.units),
                  'next':unit_binding(units,self.units)}
        for binding in releases.values():
            if binding: verify(binding)
        if releases['next']:
            verify_unit_bundle(releases['next'], units, self.units)
        folder=Path(tempfile.mkdtemp(prefix='transaction-',dir=self.state))
        journal={'version':2,'unit_directory':str(self.units),'state':'prepared','previous':previous,'next':desired,'releases':releases}
        self.save(folder,journal,'prepared'); _sync(self.state)
        try:
            stage=folder/'stage'; stage.mkdir(mode=0o700)
            for name in units: _write(stage/name,units[name].encode(),0o600)
            self.command(['systemd-analyze','verify','--man=no',*(str(stage/name) for name in units)])
            self.guard_current(journal)
            for binding in releases.values():
                if binding: verify(binding)
            if releases['next']:
                verify_unit_bundle(releases['next'], units, self.units)
            self.save(folder,journal,'applying')
            for name in MANAGED:
                if not desired[name]['exists'] and (previous[name]['exists'] or previous[name]['enabled']=='enabled'):
                    self.command(['systemctl','disable',name])
            for name in MANAGED: self.set_unit(name,desired[name])
            self.command(['systemctl','daemon-reload'])
            for name in units: self.command(['systemctl','enable',name])
            self.save(folder,journal,'complete')
        except Exception as original:
            try: self.rollback(folder)
            except Exception as recovery:
                raise RuntimeError('Appliance install failed; rollback is pending in '+str(folder)+': '+str(recovery)) from original
            raise RuntimeError('Appliance install failed; previous managed units restored: '+str(folder)) from original
        return folder



def _selection_admission(transaction):
    # Exact older inventories omit appliance_boot. Before bootstrap, retain
    # their administrative behavior; after activation, fail closed and require
    # the current administrator tooling instead of silently bypassing the gate.
    try:
        from appliance_boot import admission
    except ModuleNotFoundError as error:
        if error.name != 'appliance_boot': raise
        entry = transaction.units.parent / 'system-generators/headunit-boot-recovery'
        if entry.exists() or entry.is_symlink():
            raise ValueError('This retained installer lacks the activated boot contract; use current admission-aware administrator tooling') from error
        return
    admission(transaction)


def install(units,unit_directory,state_directory,runner=subprocess.run):
    with Transaction(unit_directory,state_directory,runner) as transaction:
        _selection_admission(transaction)
        return transaction.install(units)


def rollback(snapshot,unit_directory,state_directory,runner=subprocess.run):
    with Transaction(unit_directory,state_directory,runner) as transaction:
        _selection_admission(transaction)
        transaction.rollback(Path(snapshot))


def recover_pending(unit_directory,state_directory,runner=subprocess.run):
    """Recover interrupted unit changes without admitting a new installation."""
    with Transaction(unit_directory,state_directory,runner) as transaction:
        _selection_admission(transaction)
        transaction.recover_pending()
