# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(sys.argv.pop(1)) if len(sys.argv)>1 else ROOT/'build/headunit-display-config-check'
sys.path.insert(0,str(ROOT/'tools'))
from install_cm5_appliance import render, vehicle_arguments


class ApplianceTests(unittest.TestCase):
    def display(self, root, extra=''):
        path=root/'display.conf'
        path.write_text('main.width=1280\nmain.height=720\nmain.fps=60\nmain.panel=Main\n'
                        'main.wayland=wayland-custom\nmain.output=HDMI-A-1\n'
                        'cluster.width=1600\ncluster.height=600\ncluster.fps=30\ncluster.panel=Cluster\n'
                        'cluster.wayland=wayland-custom\ncluster.output=DP-7\n'+extra)
        path.chmod(0o600)
        return path

    def units(self, root, settings, fanout=None):
        return render(ROOT,'bench',os.getuid(),ROOT,root/'receiver.json',settings,fanout,CHECKER)

    def bus(self, root, ultra=False):
        path=root/'vehicle.json'
        consumers={'backup':str(root/'backup.sock'),'receiver':str(root/'receiver.sock')}
        if ultra: consumers['ultra']=str(root/'ultra.sock')
        path.write_text(json.dumps({'version':1,'input':str(root/'adapter.sock'),'consumers':consumers}))
        path.chmod(0o600)
        return path

    def test_local_gauges_require_authored_geometry_and_explicit_profile(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            display = self.display(root)
            settings = {'display_config': display, 'pre_phone_gauges': True}
            with self.assertRaises(ValueError):
                self.units(root, settings)
            settings['ultra_installed_ui'] = root / 'verified-ultra.conf'
            with self.assertRaises(ValueError):
                self.units(root, settings)
            display.write_text(display.read_text().replace('cluster.width=1600', 'cluster.width=1920')
                               .replace('cluster.height=600', 'cluster.height=720'))
            units = self.units(root, settings)
            self.assertIn('--local-ultra-profile ' + str(settings['ultra_installed_ui']),
                          units['headunit-cluster.service'])
            self.assertNotIn('--local-ultra-profile', units['headunit-receiver.service'])
            settings['pre_phone_gauges'] = False
            self.assertNotIn('--local-ultra-profile', self.units(root, settings)['headunit-cluster.service'])

    def test_fallback_uses_configured_output_and_no_implicit_simulation(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); settings={'display_config':self.display(root)}
            units=self.units(root,settings)
            self.assertEqual(len(units),4)
            cluster=units['headunit-cluster.service']
            self.assertIn('Environment=WAYLAND_DISPLAY=wayland-custom',cluster)
            self.assertIn('--socket wayland-custom --output DP-7',cluster)
            self.assertIn('--size 1600 600 --output DP-7 --background',cluster)
            self.assertNotIn('--simulate',cluster)
            self.assertNotIn('--vehicle-socket',cluster)
            self.assertNotIn('@',cluster)
            self.assertIn('--simulate',self.units(root,settings|{'simulate_vehicle':True})['headunit-cluster.service'])

    def test_production_parser_rejects_ambiguous_display_geometry(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve()
            for extra in ('cluster.width=1920\n','main.output=DP-7\n','unknown=1\n'):
                with self.subTest(extra=extra),self.assertRaises(subprocess.CalledProcessError):
                    self.units(root,{'display_config':self.display(root,extra)})
            path=self.display(root)
            text=path.read_text().replace('cluster.output=DP-7','cluster.output=HDMI-A-1')
            path.write_text(text)
            with self.assertRaises(ValueError): self.units(root,{'display_config':path})

    def test_physical_sources_use_distinct_consumers_and_ordered_service(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); bus=self.bus(root)
            settings={'display_config':self.display(root),'vehicle_socket':root/'receiver.sock'}
            units=self.units(root,settings,bus)
            self.assertEqual(len(units),5)
            self.assertIn('--vehicle-socket '+str(root/'backup.sock'),units['headunit-cluster.service'])
            self.assertNotIn('--simulate',units['headunit-cluster.service'])
            self.assertIn('After=headunit-vehicle.service',units['headunit-cluster.service'])
            self.assertIn('--config '+str(bus),units['headunit-vehicle.service'])
            with self.assertRaises(ValueError): self.units(root,settings)
            with self.assertRaises(ValueError): self.units(root,settings|{'simulate_vehicle':True},bus)
            with self.assertRaises(ValueError): self.units(root,settings|{'vehicle_socket':root/'wrong.sock'},bus)

    def test_ultra_profile_must_bind_its_own_consumer_without_simulation(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); bus=self.bus(root,True)
            ui=root/'installed-ui.conf'; ui.write_text('vehicleSocket='+str(root/'ultra.sock')+'\n'); ui.chmod(0o600)
            settings={'display_config':self.display(root),'vehicle_socket':root/'receiver.sock','ultra_installed_ui':ui}
            self.assertEqual(vehicle_arguments(settings,os.getuid(),bus),['--vehicle-socket',str(root/'backup.sock')])
            for text in ('simulateGauges=true\nvehicleSocket='+str(root/'ultra.sock')+'\n',
                         'vehicleSocket='+str(root/'receiver.sock')+'\n',
                         'simulateGauges=false\n'):
                ui.write_text(text)
                with self.subTest(text=text),self.assertRaises(ValueError): vehicle_arguments(settings,os.getuid(),bus)

    def test_service_path_cannot_inject_systemd_specifiers_or_commands(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve(); settings={'display_config':self.display(root)}
            for key in ('media_state','cluster_page'):
                for path in ('/tmp/field%u','/tmp/field\nExecStart=/bin/false','/tmp/field --simulate'):
                    with self.subTest(key=key,path=path),self.assertRaises(ValueError):
                        self.units(root,settings|{key:path})

    def test_optional_camera_requires_matching_main_surface_handoff(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder).resolve();profile=root/'camera.json';profile.write_text('{}')
            request=root/'camera-request';lease=root/'camera-surface'
            ui=root/'installed-ui.conf'
            ui.write_text('nativeCameraControlFile='+str(request)+'\nnativeCameraLeaseFile='+str(lease)+'\n');ui.chmod(0o600)
            settings={'display_config':self.display(root),'ultra_negotiate':True,'ultra_native_video':True,'ultra_installed_ui':ui}
            validated={'configurationValid':True,'enabled':True,'wayland':'wayland-custom','output':'HDMI-A-1','width':1280,'height':720}
            real_run=subprocess.run
            def run(command,**options):
                if command[0]=='camera-validator':
                    self.assertIn('--surface-lease-file',command)
                    self.assertEqual(command[command.index('--surface-lease-file')+1],str(lease))
                    return subprocess.CompletedProcess(command,0,stdout=json.dumps(validated))
                return real_run(command,**options)
            def units(values=settings,**kwargs):
                return render(ROOT,'bench',os.getuid(),ROOT,root/'receiver.json',values,checker=CHECKER,
                              camera_profile=profile,camera_request=request,camera_lease=lease,
                              camera_checker=Path('camera-validator'),**kwargs)
            with patch('install_cm5_appliance.subprocess.run',run):
                camera=units()['headunit-camera.service']
                self.assertIn('User=bench',camera)
                self.assertIn('--socket wayland-custom --output HDMI-A-1',camera)
                self.assertIn('--surface-lease-file '+str(lease),camera)
                self.assertIn('PartOf=headunit-compositor.service',camera)
                self.assertNotIn('headunit-receiver.service',camera)
                self.assertNotIn('headunit-cluster.service',camera)
                validated['enabled']=False
                with self.assertRaises(ValueError):units()
                validated['enabled']=True
                with self.assertRaises(ValueError):units(settings|{'ultra_negotiate':False})
                ui.write_text('nativeCameraControlFile=/tmp/wrong\nnativeCameraLeaseFile='+str(lease)+'\n')
                with self.assertRaises(ValueError):units()
                ui.write_text('nativeCameraControlFile='+str(request)+'\nnativeCameraLeaseFile='+str(lease)+'\ncameraProfile=/tmp/signed.json\n')
                with self.assertRaises(ValueError):units()

    def test_native_manager_replaces_one_receiver_and_preserves_external_cluster(self):
        with tempfile.TemporaryDirectory(prefix='pm-unit-', dir='/tmp') as folder:
            root=Path(folder).resolve(); state=root/'state'
            display=self.display(root); standard=root/'standard.conf'
            standard.write_text(display.read_text().replace('main.width=1280', 'main.width=1920'))
            profile=root/'manager.json'
            value={'schemaVersion':1,'enabled':True,'stateDirectory':str(state),
                   'standardDisplayConfig':str(standard),'autoConnectSelected':False}
            profile.write_text(json.dumps(value)); profile.chmod(0o600)
            settings={'display_config':display,'bin_dir':CHECKER.parent,'seconds':0,'media_state':state/'media.json'}
            def render_manager(values=settings, **options):
                return render(ROOT,'bench',os.getuid(),ROOT,root/'receiver.json',values,checker=CHECKER,
                              phone_manager_profile=profile,**options)
            units=render_manager()
            self.assertEqual(set(units), {'headunit-compositor.service','headunit-outputs.service',
                                         'headunit-cluster.service','headunit-receiver.service'})
            receiver=units['headunit-receiver.service']
            self.assertIn('phone_manager.py --config',receiver)
            self.assertIn('--manager-profile '+str(profile),receiver)
            self.assertNotIn('--validate-only',receiver)
            self.assertNotIn('run_lab_receiver.py',receiver)
            self.assertNotIn('ExecStartPre=/usr/bin/nmcli radio wifi on',receiver)
            self.assertIn('KillMode=control-group',receiver)
            start=next(line for line in receiver.splitlines() if line.startswith('ExecStart='))
            self.assertNotIn('run_lab_receiver.py',start)
            self.assertIn('--media-file '+str(state/'media.json'),units['headunit-cluster.service'])
            self.assertFalse(state.exists())
            with self.assertRaises(ValueError): render_manager(settings|{'fallback_cluster':True})
            with self.assertRaises(ValueError): render_manager(settings|{'media_state':root/'elsewhere.json'})
            with self.assertRaises(ValueError): render_manager(release_binding={'fixture':True})
            with self.assertRaisesRegex(ValueError, 'Standard camera handoff'):
                render_manager(camera_profile=root/'camera.json', camera_request=root/'camera-request',
                               camera_lease=root/'camera-lease')
            value['enabled']=False; profile.write_text(json.dumps(value)); profile.chmod(0o600)
            with self.assertRaises(ValueError): render_manager()

    def page_profiles(self, root):
        from receiver_configuration import load_receiver_configuration
        state=root/'state'; display=self.display(root); standard=root/'standard.conf'
        standard.write_text(display.read_text().replace('main.width=1280', 'main.width=1920'))
        manager=root/'manager.json'
        manager.write_text(json.dumps({'schemaVersion':1,'enabled':True,'stateDirectory':str(state),
                                      'standardDisplayConfig':str(standard),'autoConnectSelected':False}))
        manager.chmod(0o600)
        config=root/'receiver.json'
        config.write_text(json.dumps({'display_config':str(display),'bin_dir':str(CHECKER.parent),
                                      'seconds':0,'media_state':str(state/'media.json'),'preview_sink':'wayland-gpu',
                                      'native_page_control_file':str(state/'home-surface.request'),
                                      'native_page_lease_file':str(state/'home-surface.lease')}))
        config.chmod(0o600)
        profile=root/'page.json'
        value={'schemaVersion':2,'enabled':True,'managerSocket':str(state/'manager.sock'),
               'surfaceControlFile':str(state/'home-surface.request'),
               'surfaceLeaseFile':str(state/'home-surface.lease'),
               'waylandDisplay':'wayland-custom','output':'HDMI-A-1',
               'homeButton':{'corner':'bottom-right','margin':16,'size':56}}
        profile.write_text(json.dumps(value)); profile.chmod(0o600)
        return config, load_receiver_configuration(config), manager, profile, value

    def test_optional_home_page_uses_main_route_and_survives_receiver_failure(self):
        with tempfile.TemporaryDirectory(prefix='page-unit-',dir='/tmp') as folder:
            root=Path(folder).resolve()
            config,settings,manager,profile,_=self.page_profiles(root)
            units=render(ROOT,'bench',os.getuid(),ROOT,config,settings,checker=CHECKER,
                         phone_manager_profile=manager,phone_page_profile=profile)
            self.assertEqual(len(units),5)
            page=units['headunit-phone-page.service']
            self.assertIn('User=bench',page)
            self.assertIn('Environment=WAYLAND_DISPLAY=wayland-custom',page)
            self.assertIn('Environment=GDK_BACKEND=wayland',page)
            self.assertIn('--socket wayland-custom --output HDMI-A-1',page)
            self.assertIn('--profile '+str(profile)+' --manager-profile '+str(manager)+
                          ' --receiver-config '+str(config),page)
            self.assertNotIn('--validate-only',page)
            self.assertEqual(sum(line.startswith('ExecStart=') for line in page.splitlines()), 1)
            self.assertIn('Wants=headunit-outputs.service headunit-receiver.service',page)
            self.assertIn('Type=notify\nNotifyAccess=main\nTimeoutStartSec=15',page)
            page_after=next(line for line in page.splitlines() if line.startswith('After='))
            self.assertNotIn('headunit-receiver.service',page_after)
            receiver_after=next(line for line in units['headunit-receiver.service'].splitlines() if line.startswith('After='))
            self.assertNotIn('headunit-cluster.service',receiver_after)
            self.assertNotIn('headunit-compositor.service',receiver_after)
            self.assertNotIn('NetworkManager.service',receiver_after)
            self.assertIn('NetworkManager.service', units['headunit-receiver.service'].split('Wants=', 1)[1].splitlines()[0])
            self.assertIn('After=headunit-phone-page.service',
                          units['headunit-cluster.service'])
            self.assertNotIn('Wants=headunit-phone-page.service',
                             self.units(root,{'display_config':self.display(root)})['headunit-cluster.service'])
            self.assertIn('PartOf=headunit-compositor.service',page)
            for line in page.splitlines():
                if line.startswith(('Requires=','PartOf=')):
                    self.assertNotIn('headunit-receiver',line)
                    self.assertNotIn('headunit-cluster',line)
            self.assertNotIn('@',page)
            self.assertFalse((root/'state').exists())
            self.assertIn('--output DP-7',units['headunit-cluster.service'])
            legacy=render(ROOT,'bench',os.getuid(),ROOT,config,settings,checker=CHECKER)
            self.assertEqual(len(legacy),4)
            self.assertNotIn('headunit-phone-page.service',legacy)
            self.assertNotIn('phone_manager.py',legacy['headunit-receiver.service'])

    def test_home_page_admission_rejects_missing_manager_wrong_routes_and_mixed_settings(self):
        with tempfile.TemporaryDirectory(prefix='page-unit-',dir='/tmp') as folder:
            root=Path(folder).resolve()
            config,settings,manager,profile,value=self.page_profiles(root)
            def units(values=settings,**options):
                return render(ROOT,'bench',os.getuid(),ROOT,config,values,checker=CHECKER,
                              phone_manager_profile=manager,phone_page_profile=profile,**options)
            with self.assertRaisesRegex(ValueError,'supervised phone manager'):
                render(ROOT,'bench',os.getuid(),ROOT,config,settings,checker=CHECKER,phone_page_profile=profile)
            for changed in ({'enabled':False},{'output':'DP-7'},{'waylandDisplay':'wayland-0'},
                            {'managerSocket':str(root/'another.sock')}):
                profile.write_text(json.dumps(value|changed))
                with self.subTest(changed=changed),self.assertRaises(ValueError): units()
            profile.write_text(json.dumps(value))
            with self.assertRaisesRegex(ValueError,'same receiver configuration'):
                units(settings|{'simulate_vehicle':True})
            with self.assertRaisesRegex(ValueError,'release binding'):
                units(release_binding={'fixture':True})
            with patch('appliance_release.verify', return_value={'version':1}):
                with self.assertRaisesRegex(ValueError,'Version 1 sealed releases'):
                    units(release_binding={'fixture':True})
            self.assertFalse((root/'state').exists())

    def test_home_page_rejects_signed_and_native_camera_inputs(self):
        with tempfile.TemporaryDirectory(prefix='page-unit-',dir='/tmp') as folder:
            from receiver_configuration import load_receiver_configuration
            root=Path(folder).resolve()
            config,settings,manager,profile,_=self.page_profiles(root)
            original=json.loads(config.read_text())
            ui=root/'installed-ui.conf'
            def units():
                return render(ROOT,'bench',os.getuid(),ROOT,config,load_receiver_configuration(config),checker=CHECKER,
                              phone_manager_profile=manager,phone_page_profile=profile)
            config.write_text(json.dumps(original|{'camera_manual_profile':str(root/'manual.json')}))
            with self.assertRaises(ValueError): units()
            for key in ('cameraProfile','nativeCameraControlFile','nativeCameraLeaseFile'):
                ui.write_text(key+'='+str(root/'camera')+'\n'); ui.chmod(0o600)
                config.write_text(json.dumps(original|{'ultra_installed_ui':str(ui)}))
                with self.subTest(key=key),self.assertRaises(ValueError): units()
            ui.write_text('cameraProfile=\nnativeCameraControlFile=\nnativeCameraLeaseFile=\n')
            self.assertIn('headunit-phone-page.service',units())
            self.assertFalse((root/'state').exists())


if __name__=='__main__': unittest.main()
