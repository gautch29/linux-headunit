# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import appliance_transaction as module
from appliance_transaction import MANAGED,MAX_JOURNAL,MAX_SNAPSHOTS,Transaction,install,rollback,recover_pending


def body(label):
    return '[Unit]\nDescription='+label+'\n[Service]\nExecStart=/bin/true\n[Install]\nWantedBy=multi-user.target\n'


def stat_mode(path):
    return path.stat().st_mode&0o7777


class System:
    def __init__(self,directory):
        self.directory=directory; self.states={}; self.vendor={}; self.calls=[]; self.fail=None; self.failed=False; self.verify_hook=None
        self.active={}; self.jobs={}; self.gate={'ActiveState':'inactive','SubState':'dead','Result':'success'}

    def __call__(self,args,check=True,**_):
        self.calls.append(args)
        if self.fail and not self.failed and self.fail(args):
            self.failed=True
            raise subprocess.CalledProcessError(1,args,stderr='injected failure')
        if args[0]=='systemd-analyze':
            assert args[1:3]==['verify','--man=no']
            assert all(Path(name).is_file() and Path(name).name in MANAGED for name in args[3:])
            if self.verify_hook: self.verify_hook()
            return subprocess.CompletedProcess(args,0,stdout='')
        assert args[0]=='systemctl'
        if args[1]=='show':
            name=args[-1]
            fields=[arg.split('=',1)[1] for arg in args if arg.startswith('--property=')]
            values=self.gate if name=='headunit-boot-recovery.service' else {'ActiveState':self.active.get(name,'inactive')}
            output='\n'.join(values[field] if '--value' in args else field+'='+values[field] for field in fields)+'\n'
            return subprocess.CompletedProcess(args,0,stdout=output)
        if args[1]=='list-jobs':
            return subprocess.CompletedProcess(args,0,stdout=''.join(number+' '+name+' start waiting\n' for name,number in self.jobs.items()))
        if args[1]=='is-enabled':
            value=(self.states.get(args[2],'disabled') if (self.directory/args[2]).exists()
                   else self.vendor.get(args[2],'not-found'))
            return subprocess.CompletedProcess(args,0 if value=='enabled' else 1,stdout=value+'\n')
        if args[1]=='enable':
            assert (self.directory/args[2]).exists()
            self.states[args[2]]='enabled'
        elif args[1]=='disable': self.states[args[2]]='disabled'
        else: assert args==['systemctl','daemon-reload']
        return subprocess.CompletedProcess(args,0,stdout='')


class PowerLoss(BaseException): pass


class Tests(unittest.TestCase):
    def fixture(self,folder):
        root=Path(folder).resolve(); directory=root/'units'; directory.mkdir(mode=0o700)
        state=root/'state'; system=System(directory)
        # Two prior managed files with different modes and boot enable states.
        old={MANAGED[0]:body('old compositor').encode(),MANAGED[2]:body('old cluster').encode()}
        for name,data in old.items():
            (directory/name).write_bytes(data); (directory/name).chmod(0o640 if name==MANAGED[0] else 0o644)
        system.states[MANAGED[0]]='enabled'; system.states[MANAGED[2]]='disabled'
        unrelated=directory/'unrelated.service'; unrelated.write_text('untouched'); unrelated.chmod(0o644)
        new={name:body('new '+name) for name in MANAGED[:4]}
        return directory,state,system,old,new

    def assert_old(self,directory,system,old):
        for name in MANAGED:
            if name in old:
                self.assertEqual((directory/name).read_bytes(),old[name])
                self.assertEqual((directory/name).stat().st_mode&0o777,0o640 if name==MANAGED[0] else 0o644)
            else: self.assertFalse((directory/name).exists())
        self.assertEqual(system.states[MANAGED[0]],'enabled')
        self.assertEqual(system.states[MANAGED[2]],'disabled')
        self.assertEqual((directory/'unrelated.service').read_text(),'untouched')
        self.assertFalse(any(call[1] in ('start','stop','restart','try-restart') for call in system.calls if call[0]=='systemctl'))

    def test_success_and_explicit_exact_rollback(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            snapshot=install(new,directory,state,system)
            self.assertEqual(json.loads((snapshot/'journal.json').read_text())['state'],'complete')
            self.assertEqual(snapshot.stat().st_mode&0o777,0o700)
            self.assertEqual((snapshot/'journal.json').stat().st_mode&0o777,0o600)
            for name,text in new.items():
                self.assertEqual((directory/name).read_text(),text)
                self.assertEqual(system.states[name],'enabled')
            rollback(snapshot,directory,state,system)
            self.assert_old(directory,system,old)
            self.assertEqual(json.loads((snapshot/'journal.json').read_text())['state'],'rolled_back')

    def test_reload_enable_verify_and_partial_write_failures_preserve_old_bundle(self):
        for failure in ('verify','reload','enable','write'):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as folder:
                directory,state,system,old,new=self.fixture(folder)
                if failure=='verify': system.fail=lambda args:args[0]=='systemd-analyze'
                elif failure=='reload': system.fail=lambda args:args==['systemctl','daemon-reload']
                elif failure=='enable': system.fail=lambda args:args==['systemctl','enable',MANAGED[3]]
                writer=module._write; count=0
                def write(path,data,mode):
                    nonlocal count
                    if path.parent==directory:
                        count+=1
                        if failure=='write' and count==3: raise OSError('injected partial bundle write')
                    return writer(path,data,mode)
                with patch.object(module,'_write',write),self.assertRaises(RuntimeError):
                    install(new,directory,state,system)
                self.assert_old(directory,system,old)
                journals=list(state.glob('transaction-*/journal.json'))
                self.assertEqual(len(journals),1)
                self.assertEqual(json.loads(journals[0].read_text())['state'],'rolled_back')

    def test_interrupted_write_and_enable_are_recovered_before_next_install(self):
        for boundary in ('write','enable'):
            with self.subTest(boundary=boundary),tempfile.TemporaryDirectory() as folder:
                directory,state,system,old,new=self.fixture(folder)
                writer=module._write; count=0
                def write(path,data,mode):
                    nonlocal count
                    result=writer(path,data,mode)
                    if path.parent==directory:
                        count+=1
                        if boundary=='write' and count==2: raise PowerLoss()
                    return result
                def command(args,**kwargs):
                    result=system(args,**kwargs)
                    if boundary=='enable' and args==['systemctl','enable',MANAGED[1]]: raise PowerLoss()
                    return result
                with patch.object(module,'_write',write),self.assertRaises(PowerLoss):
                    install(new,directory,state,command)
                self.assertEqual(json.loads(next(state.glob('transaction-*/journal.json')).read_text())['state'],'applying')
                with Transaction(directory,state,system) as transaction: transaction.recover_pending()
                self.assert_old(directory,system,old)
                # The same automatic recovery is also called at install admission.
                install(new,directory,state,system)
                self.assertEqual((directory/MANAGED[0]).read_text(),new[MANAGED[0]])

    def test_failed_rollback_remains_pending_and_can_be_retried(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            snapshot=install(new,directory,state,system)
            system.fail=lambda args:args==['systemctl','daemon-reload']
            with self.assertRaises(subprocess.CalledProcessError): rollback(snapshot,directory,state,system)
            self.assertEqual(json.loads((snapshot/'journal.json').read_text())['state'],'rollback_pending')
            rollback(snapshot,directory,state,system)
            self.assert_old(directory,system,old)

    def test_optional_vehicle_service_is_disabled_before_removal(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            (directory/MANAGED[4]).write_text(body('old broker')); (directory/MANAGED[4]).chmod(0o644)
            system.states[MANAGED[4]]='enabled'
            snapshot=install(new,directory,state,system)
            self.assertFalse((directory/MANAGED[4]).exists())
            self.assertEqual(system.states[MANAGED[4]],'disabled')
            rollback(snapshot,directory,state,system)
            self.assertEqual((directory/MANAGED[4]).read_text(),body('old broker'))
            self.assertEqual(system.states[MANAGED[4]],'enabled')

    def test_optional_camera_service_is_restored_after_bundle_rollback(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            camera='headunit-camera.service'
            (directory/camera).write_text(body('old camera'));(directory/camera).chmod(0o644)
            system.states[camera]='enabled'
            snapshot=install(new,directory,state,system)
            self.assertFalse((directory/camera).exists())
            self.assertEqual(system.states[camera],'disabled')
            rollback(snapshot,directory,state,system)
            self.assertEqual((directory/camera).read_text(),body('old camera'))
            self.assertEqual(system.states[camera],'enabled')

    def test_legacy_snapshot_does_not_own_a_later_camera_service(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            snapshot=install(new,directory,state,system)
            journal=json.loads((snapshot/'journal.json').read_text())
            # Actual previous format: both recorded bundles contain five names.
            camera='headunit-camera.service'
            for key in ('previous','next'):
                journal[key]={name:record for name,record in journal[key].items() if name in module.LEGACY_MANAGED}
            (snapshot/'journal.json').write_text(json.dumps(journal))
            (directory/camera).write_text(body('later camera'));(directory/camera).chmod(0o644)
            system.states[camera]='enabled';system.calls.clear()
            rollback(snapshot,directory,state,system)
            self.assertEqual((directory/camera).read_text(),body('later camera'))
            self.assertEqual(system.states[camera],'enabled')
            self.assertFalse(any(camera in call for call in system.calls))
            for name,data in old.items():self.assertEqual((directory/name).read_bytes(),data)

    def test_old_five_and_six_unit_journals_preserve_later_page_and_camera_generations(self):
        for version in (1,2):
            for scope in (module.LEGACY_MANAGED,module.CAMERA_MANAGED):
                for enabled in ('enabled','disabled','static'):
                    with self.subTest(version=version,scope=len(scope),enabled=enabled),tempfile.TemporaryDirectory() as folder:
                        directory,state,system,old,new=self.fixture(folder)
                        snapshot=install(new,directory,state,system)
                        journal_path=snapshot/'journal.json'
                        journal=json.loads(journal_path.read_text())
                        journal['version']=version
                        if version==1:journal.pop('releases')
                        for key in ('previous','next'):
                            journal[key]={name:record for name,record in journal[key].items() if name in scope}
                        journal_path.write_text(json.dumps(journal))
                        unrelated={}
                        for name in set(MANAGED)-set(scope):
                            path=directory/name
                            data=body('later independently owned '+name).encode()
                            path.write_bytes(data);path.chmod(0o640)
                            system.states[name]=enabled
                            unrelated[name]=(data,stat_mode(path),path.stat().st_ino)
                        system.calls.clear()
                        rollback(snapshot,directory,state,system)
                        for name,(data,mode,inode) in unrelated.items():
                            path=directory/name
                            self.assertEqual((path.read_bytes(),stat_mode(path),path.stat().st_ino),(data,mode,inode))
                            self.assertEqual(system.states[name],enabled)
                            self.assertFalse(any(name in call for call in system.calls))
                        for name,data in old.items():self.assertEqual((directory/name).read_bytes(),data)
                        self.assertEqual((directory/'unrelated.service').read_text(),'untouched')

    def test_journal_scope_is_exact_not_any_subset_of_seven_services(self):
        for removed in (MANAGED[0],'headunit-camera.service'):
            with self.subTest(removed=removed),tempfile.TemporaryDirectory() as folder:
                directory,state,system,old,new=self.fixture(folder)
                snapshot=install(new,directory,state,system)
                journal_path=snapshot/'journal.json';journal=json.loads(journal_path.read_text())
                for key in ('previous','next'):journal[key].pop(removed)
                journal_path.write_text(json.dumps(journal));system.calls.clear()
                before={path.name:(path.read_bytes(),stat_mode(path)) for path in directory.iterdir()}
                with self.assertRaisesRegex(ValueError,'unmanaged or missing'):rollback(snapshot,directory,state,system)
                self.assertEqual(system.calls,[])
                self.assertEqual({path.name:(path.read_bytes(),stat_mode(path)) for path in directory.iterdir()},before)

    def test_seven_unit_page_install_removal_and_exact_rollback(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            page='headunit-phone-page.service';text=body('resident page')
            snapshot=install(new|{page:text},directory,state,system)
            journal=json.loads((snapshot/'journal.json').read_text())
            self.assertEqual(set(journal['previous']),set(MANAGED))
            self.assertEqual(set(journal['next']),set(MANAGED))
            self.assertEqual(len(MANAGED),7)
            self.assertEqual((directory/page).read_text(),text)
            self.assertEqual(stat_mode(directory/page),0o644)
            self.assertEqual(system.states[page],'enabled')
            # A subsequent full install owns removal and retains the exact
            # previous file mode, unlike rollback of an older journal scope.
            (directory/page).chmod(0o640)
            def removing(args,**options):
                if args==['systemctl','disable',page]:self.assertTrue((directory/page).is_file())
                return system(args,**options)
            removed=install(new,directory,state,removing)
            self.assertFalse((directory/page).exists())
            self.assertEqual(system.states[page],'disabled')
            rollback(removed,directory,state,system)
            self.assertEqual((directory/page).read_text(),text)
            self.assertEqual(stat_mode(directory/page),0o640)
            self.assertEqual(system.states[page],'enabled')
            self.assertEqual((directory/'unrelated.service').read_text(),'untouched')
            self.assertFalse(any(call[1] in ('start','stop','restart','try-restart') for call in system.calls if call[0]=='systemctl'))

    def test_new_page_install_and_removal_failures_restore_previous_scope(self):
        for present in (False,True):
            for failure in ('verify','reload','enable','write'):
                with self.subTest(present=present,failure=failure),tempfile.TemporaryDirectory() as folder:
                    directory,state,system,old,new=self.fixture(folder)
                    page='headunit-phone-page.service'
                    if present:
                        old[page]=body('old resident page').encode()
                        (directory/page).write_bytes(old[page]);(directory/page).chmod(0o640)
                        system.states[page]='enabled'
                    desired=new if present else new|{page:body('new resident page')}
                    if failure=='verify':system.fail=lambda args:args[0]=='systemd-analyze'
                    elif failure=='reload':system.fail=lambda args:args==['systemctl','daemon-reload']
                    elif failure=='enable':system.fail=lambda args:args==['systemctl','enable',MANAGED[3] if present else page]
                    apply=Transaction.set_unit;failed=False
                    def write(owner,name,record):
                        nonlocal failed
                        apply(owner,name,record)
                        if failure=='write' and name==page and not failed:
                            failed=True;raise OSError('injected resident-page write failure')
                    with patch.object(Transaction,'set_unit',write),self.assertRaises(RuntimeError):
                        install(desired,directory,state,system)
                    for name in MANAGED:
                        if name in old:
                            self.assertEqual((directory/name).read_bytes(),old[name])
                            self.assertEqual(stat_mode(directory/name),0o640 if name in (MANAGED[0],page) else 0o644)
                        else:self.assertFalse((directory/name).exists())
                    if present:self.assertEqual(system.states[page],'enabled')
                    self.assertEqual((directory/'unrelated.service').read_text(),'untouched')
                    self.assertEqual(json.loads(next(state.glob('transaction-*/journal.json')).read_text())['state'],'rolled_back')

    def test_interrupted_page_write_is_recovered_from_seven_unit_journal(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            page='headunit-phone-page.service';apply=Transaction.set_unit
            def interrupted(owner,name,record):
                apply(owner,name,record)
                if name==page:raise PowerLoss()
            with patch.object(Transaction,'set_unit',interrupted),self.assertRaises(PowerLoss):
                install(new|{page:body('interrupted page')},directory,state,system)
            self.assertTrue((directory/page).is_file())
            journal=json.loads(next(state.glob('transaction-*/journal.json')).read_text())
            self.assertEqual(journal['state'],'applying');self.assertEqual(set(journal['previous']),set(MANAGED))
            with Transaction(directory,state,system) as transaction:transaction.recover_pending()
            self.assert_old(directory,system,old)

    def test_external_edits_symlinks_and_unmanaged_journal_names_are_protected(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            snapshot=install(new,directory,state,system)
            (directory/MANAGED[0]).write_text(body('later manual edit'))
            before=len(system.calls)
            with self.assertRaises(ValueError): rollback(snapshot,directory,state,system)
            self.assertEqual(len(system.calls),before)
            self.assertEqual((directory/MANAGED[0]).read_text(),body('later manual edit'))
            (directory/MANAGED[0]).unlink(); (directory/MANAGED[0]).symlink_to(directory/'unrelated.service')
            with self.assertRaises(OSError): rollback(snapshot,directory,state,system)
            self.assertEqual((directory/'unrelated.service').read_text(),'untouched')
            (directory/MANAGED[0]).unlink(); (directory/MANAGED[0]).write_text(new[MANAGED[0]])
            journal=json.loads((snapshot/'journal.json').read_text())
            journal['previous']['unrelated.service']=journal['previous'].pop(MANAGED[0])
            (snapshot/'journal.json').write_text(json.dumps(journal))
            with self.assertRaises(ValueError): rollback(snapshot,directory,state,system)
            self.assertEqual((directory/'unrelated.service').read_text(),'untouched')

    def test_edit_during_verification_is_rejected_before_application(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            system.verify_hook=lambda:(directory/MANAGED[0]).write_text(body('changed during verify'))
            with self.assertRaises(RuntimeError): install(new,directory,state,system)
            self.assertEqual((directory/MANAGED[0]).read_text(),body('changed during verify'))
            self.assertFalse(any(c[:2] in (['systemctl','enable'],['systemctl','disable'],['systemctl','daemon-reload']) for c in system.calls))

    def test_owned_private_journal_bounds_hash_and_exclusive_lock(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            with Transaction(directory,state,system):
                with self.assertRaises(BlockingIOError):
                    with Transaction(directory,state,system): pass
            snapshot=install(new,directory,state,system)
            journal_path=snapshot/'journal.json'; original=journal_path.read_bytes()
            journal=json.loads(original); journal['previous'][MANAGED[0]]['sha256']='0'*64
            journal_path.write_text(json.dumps(journal))
            with self.assertRaises(ValueError): rollback(snapshot,directory,state,system)
            journal_path.write_bytes(b' '* (MAX_JOURNAL+1))
            with self.assertRaises(ValueError): rollback(snapshot,directory,state,system)
            journal_path.write_bytes(original); journal_path.chmod(0o644)
            with self.assertRaises(ValueError): rollback(snapshot,directory,state,system)
            journal_path.chmod(0o600)
            state.chmod(0o755)
            with self.assertRaises(ValueError): rollback(snapshot,directory,state,system)

    def test_indirect_masked_and_runtime_enable_states_are_rejected_before_changes(self):
        for value in ('indirect','masked','enabled-runtime','linked'):
            with self.subTest(state=value),tempfile.TemporaryDirectory() as folder:
                directory,state,system,old,new=self.fixture(folder)
                system.states[MANAGED[0]]=value
                with self.assertRaises(ValueError): install(new,directory,state,system)
                self.assertEqual((directory/MANAGED[0]).read_bytes(),old[MANAGED[0]])
                self.assertFalse(any(c[0]=='systemd-analyze' for c in system.calls))

    def test_vendor_loaded_name_without_local_file_is_rejected_before_changes(self):
        for value in ('enabled','disabled','static'):
            with self.subTest(state=value),tempfile.TemporaryDirectory() as folder:
                directory,state,system,old,new=self.fixture(folder)
                system.vendor[MANAGED[1]]=value
                with self.assertRaisesRegex(ValueError,'outside the local unit directory'):
                    install(new,directory,state,system)
                self.assertEqual((directory/MANAGED[0]).read_bytes(),old[MANAGED[0]])
                self.assertFalse((directory/MANAGED[1]).exists())
                self.assertFalse(any(c[0]=='systemd-analyze' for c in system.calls))

    def test_snapshot_limit_blocks_new_install_but_not_explicit_rollback(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            snapshot=install(new,directory,state,system)
            # Valid private empty folders model a crash before the first journal.
            for number in range(MAX_SNAPSHOTS-1):
                (state/('transaction-%08d'%number)).mkdir(mode=0o700)
            before=len(system.calls)
            with self.assertRaisesRegex(ValueError,'Snapshot limit reached'):
                install(new,directory,state,system)
            self.assertEqual(len(system.calls),before)
            self.assertEqual(len(list(state.glob('transaction-*'))),MAX_SNAPSHOTS)
            rollback(snapshot,directory,state,system)
            self.assert_old(directory,system,old)

    def test_rollback_cli_does_not_require_working_receiver_or_display_configuration(self):
        import install_cm5_appliance as installer
        with tempfile.TemporaryDirectory() as folder:
            snapshot=Path(folder).resolve()/'transaction-12345678'
            argv=['installer','--rollback',str(snapshot),'--transaction-directory',str(snapshot.parent)]
            with patch.object(sys,'argv',argv),patch.object(installer.os,'geteuid',return_value=0), \
                 patch.object(installer,'rollback_units') as restore,contextlib.redirect_stdout(io.StringIO()):
                installer.main()
            restore.assert_called_once_with(snapshot,Path('/etc/systemd/system'),snapshot.parent)

    def test_recovery_only_resumes_failed_cleanup_and_then_is_a_no_op(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            apply=Transaction.set_unit
            def interrupted(owner,name,record):
                apply(owner,name,record)
                if name==MANAGED[1]:raise PowerLoss()
            with patch.object(Transaction,'set_unit',interrupted),self.assertRaises(PowerLoss):
                install(new,directory,state,system)
            journal=next(state.glob('transaction-*/journal.json'))
            system.fail=lambda args:args==['systemctl','daemon-reload']
            with self.assertRaises(subprocess.CalledProcessError):recover_pending(directory,state,system)
            self.assertEqual(json.loads(journal.read_bytes())['state'],'rollback_pending')
            recover_pending(directory,state,system)
            self.assert_old(directory,system,old)
            before=journal.read_bytes();system.calls.clear()
            recover_pending(directory,state,system)
            self.assertEqual(journal.read_bytes(),before);self.assertEqual(system.calls,[])
            self.assertEqual(len(list(state.glob('transaction-*'))),1)

    def test_recovery_only_preserves_complete_installations_and_exclusive_owner(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            snapshot=install(new,directory,state,system)
            before={path.name:path.read_bytes() for path in directory.iterdir()}
            journal=(snapshot/'journal.json').read_bytes();system.calls.clear()
            with Transaction(directory,state,system):
                with self.assertRaises(BlockingIOError):recover_pending(directory,state,system)
            recover_pending(directory,state,system)
            self.assertEqual(system.calls,[])
            self.assertEqual((snapshot/'journal.json').read_bytes(),journal)
            self.assertEqual({path.name:path.read_bytes() for path in directory.iterdir()},before)

    def test_recovery_only_rejects_damaged_or_excessive_journals_without_unit_commands(self):
        for damage in ('oversize','hash','mode','count'):
            with self.subTest(damage=damage),tempfile.TemporaryDirectory() as folder:
                directory,state,system,old,new=self.fixture(folder)
                snapshot=install(new,directory,state,system);path=snapshot/'journal.json'
                journal=json.loads(path.read_bytes());journal['state']='applying'
                if damage=='hash':journal['previous'][MANAGED[0]]['sha256']='0'*64
                path.write_text(json.dumps(journal))
                if damage=='oversize':path.write_bytes(b' '*(MAX_JOURNAL+1))
                if damage=='mode':path.chmod(0o644)
                if damage=='count':
                    for number in range(MAX_SNAPSHOTS):
                        (state/('transaction-%08d'%number)).mkdir(mode=0o700)
                before={p.name:p.read_bytes() for p in directory.iterdir()};system.calls.clear()
                with self.assertRaises(ValueError):recover_pending(directory,state,system)
                self.assertEqual(system.calls,[])
                self.assertEqual({p.name:p.read_bytes() for p in directory.iterdir()},before)

    def test_recovery_cli_runs_owner_without_configuration_or_snapshot_selector(self):
        import install_cm5_appliance as installer
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            def interrupted(args,**options):
                result=system(args,**options)
                if args==['systemctl','enable',MANAGED[1]]:raise PowerLoss()
                return result
            with self.assertRaises(PowerLoss):install(new,directory,state,interrupted)
            # Only root admission, the unit-directory boundary and systemctl
            # are fixtures. The actual CLI, journal owner and recovery run.
            def recover(units,selected):
                self.assertEqual(units,Path('/etc/systemd/system'));self.assertEqual(selected,state)
                recover_pending(directory,selected,system)
            with patch.object(sys,'argv',['installer','--recover-pending','--transaction-directory',str(state)]), \
                 patch.object(installer.os,'geteuid',return_value=0),patch.object(installer,'recover_units',side_effect=recover), \
                 patch.object(installer,'render',side_effect=AssertionError('Recovery admitted a new bundle')), \
                 patch.object(installer.pwd,'getpwnam',side_effect=AssertionError('Recovery loaded the display account')), \
                 patch('receiver_configuration.load_receiver_configuration',side_effect=AssertionError('Recovery loaded receiver settings')), \
                 contextlib.redirect_stdout(io.StringIO()):
                installer.main()
            self.assert_old(directory,system,old)

    def test_recovery_only_refuses_conflicting_pending_generations(self):
        with tempfile.TemporaryDirectory() as folder:
            directory,state,system,old,new=self.fixture(folder)
            first=install(new,directory,state,system);earlier=state/'transaction-00000000';first.rename(earlier)
            second=install({name:text.replace('new ','later ') for name,text in new.items()},directory,state,system)
            later=state/'transaction-11111111';second.rename(later)
            # A valid installer cannot create this history: it must finish
            # recovery before accepting the next generation. Never guess which
            # conflicting pending snapshot now owns the current unit bytes.
            for snapshot in (earlier,later):
                path=snapshot/'journal.json';journal=json.loads(path.read_bytes());journal['state']='applying'
                path.write_text(json.dumps(journal))
            before={p.name:p.read_bytes() for p in directory.iterdir()};system.calls.clear()
            with self.assertRaisesRegex(ValueError,'changed outside this transaction'):
                recover_pending(directory,state,system)
            self.assertEqual(system.calls,[])
            self.assertEqual({p.name:p.read_bytes() for p in directory.iterdir()},before)

    def test_recovery_cli_refuses_nonroot_or_installation_arguments_before_owner(self):
        import install_cm5_appliance as installer
        extra=[['--install'],['--rollback','/missing/transaction-12345678'],['--user','fixture'],
               ['--repo','/missing'],['--receiver-config','/missing'],['--phone-manager-profile','/missing'],
               ['--phone-page-profile','/missing'],['--vehicle-fanout-config','/missing'],
               ['--camera-profile','/missing'],['--camera-control-file','/missing'],['--camera-surface-lease-file','/missing']]
        for uid,arguments in [(1,[])]+[(0,value) for value in extra]:
            with self.subTest(uid=uid,arguments=arguments),patch.object(sys,'argv',['installer','--recover-pending',*arguments]), \
                 patch.object(installer.os,'geteuid',return_value=uid),patch.object(installer,'recover_units') as recover, \
                 contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as error:
                installer.main()
            self.assertEqual(error.exception.code,2);recover.assert_not_called()


if __name__=='__main__': unittest.main()
