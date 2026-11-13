from pathlib import Path
import argparse,hashlib,importlib.util,json,sys,tempfile,unittest
sys.dont_write_bytecode=True
D=Path(__file__).parent
spec=importlib.util.spec_from_file_location('corrected',D/'run.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
TEMP=tempfile.TemporaryDirectory(prefix='sema-recursion-process-controls-')
BASE=Path(TEMP.name)
VALID={'exit':0,'timed_out':False,'cleanup_complete':True,'abandoned_pipes':False,'stdout':b'output\n','stderr':b''}
counter=0

def setup(label):
 global counter
 counter+=1;root=BASE/(str(counter)+'-'+label);root.mkdir()
 m.HERE=root/'repo/selfhost/tests/sema_recursion';m.SELFHOST=root/'repo/selfhost/src/wzcc';m.STDLIB=root/'repo/stdlib';m.EXPANDING=root/'repo/tests/generics/fixtures/expanding-recursion'
 for p in [m.HERE/'src',m.SELFHOST/'sema',m.STDLIB,m.EXPANDING/'src']:p.mkdir(parents=True,exist_ok=True)
 for p in [m.HERE/'run.py',m.HERE/'src/main.wz',m.SELFHOST/'sema/a_instantiate.wz',m.STDLIB/'text.wz',m.EXPANDING/'lang.toml',m.EXPANDING/'src/main.wz']:p.write_text('source bytes\n')
 work=root/'work';(work/'component/src').mkdir(parents=True);(work/'fixtures').mkdir()
 (work/'component/src/wzcc').symlink_to(m.SELFHOST,target_is_directory=True)
 (work/'component/src/main.wz').write_text('driver bytes\n');(work/'component/lang.toml').write_text('manifest\n');(work/'fixtures/main.wz').write_text('fixture bytes\n')
 stage0=root/'stage0';stage0.write_bytes(b'stage0 bytes');program=root/'program';program.write_bytes(b'program bytes')
 report={'work_dir':str(work),'stage0':str(stage0),'commands':[],'status':'running','program_sha256':{str(program):m.sha256(program)}}
 report['inputs_before']=m.input_snapshot(work,stage0);m.save_report(report)
 return work,program,report

class Controls(unittest.TestCase):
 def test_real_wrapper_positive_record_and_streams(self):
  work,program,report=setup('positive')
  def process(*args):
   on_disk=json.loads((work/'report.json').read_text());self.assertEqual(on_disk['status'],'running');self.assertEqual(on_disk['commands'][0]['status'],'running');self.assertIn('executable_sha256_before',on_disk['commands'][0]);return dict(VALID)
  m._run_process_impl=process
  self.assertEqual(m.run([program],'ok',work,{},1,report),b'output\n')
  d=json.loads((work/'report.json').read_text());r=d['commands'][0]
  self.assertEqual(r['status'],'passed');self.assertEqual(r['inputs_before'],r['inputs_after']);self.assertEqual(r['executable_sha256_before'],r['executable_sha256_after']);self.assertEqual(m.sha256(work/'ok.stdout'),r['stdout_sha256'])
 def test_bad_types_and_missing_fields_fail_and_persist(self):
  variations=[{'exit':False},{'timed_out':0},{'cleanup_complete':1},{'abandoned_pipes':0},{'stderr':b'unexpected stderr\n'}]
  variations += [{'missing':k}for k in ['exit','timed_out','cleanup_complete','abandoned_pipes']]
  for variation in variations:
   with self.subTest(variation=variation):
    work,program,report=setup('bad-type');value={**VALID,**{k:v for k,v in variation.items()if k!='missing'}}
    if 'missing'in variation:del value[variation['missing']]
    m._run_process_impl=lambda *args,v=value:v
    with self.assertRaises((RuntimeError,KeyError)):m.run([program],'bad',work,{},1,report)
    d=json.loads((work/'report.json').read_text());self.assertEqual(d['status'],'failed');self.assertEqual(d['commands'][0]['status'],'failed');self.assertTrue((work/'bad.stdout').exists())
 def test_reference_exit_one_only(self):
  for exit in [0,1]:
   with self.subTest(exit=exit):
    work,program,report=setup('reference')
    diagnostic={'code':'E_INSTANTIATION_LIMIT','phase':'type','severity':'error','message':'generic instantiation depth budget exceeded (48)','primary':{'path':str(m.EXPANDING/'src/main.wz'),'start_byte':71,'end_byte':101},'related':[],'fixits':[],'notes':[]}
    value={**VALID,'exit':exit,'stdout':json.dumps({'status':'error','diagnostics':[diagnostic]}).encode()};m._run_process_impl=lambda *args,v=value:v
    if exit==0:
     with self.assertRaises(RuntimeError):m.reference_main(program,work,{},1,report)
    else:self.assertEqual(m.reference_main(program,work,{},1,report)['begin'],71)
 def test_exact_path_positive_and_wrong_root_rejected(self):
  setup('path');actual={**m.EXPECTED_MAIN,'path':str((m.EXPANDING/'src/main.wz').resolve())};m.check_main('STATE Failed has=0 diags=1',[actual])
  actual['path']='/different/repository/tests/generics/fixtures/expanding-recursion/src/main.wz'
  with self.assertRaises(RuntimeError):m.check_main('STATE Failed has=0 diags=1',[actual])
 def test_input_changes_before_and_during_and_program_change(self):
  for mutation in ['source-before','source-during','stdlib-during','driver-during','fixture-during','stage0-during','program-during','new-source-during']:
   with self.subTest(mutation=mutation):
    work,program,report=setup('mutation');called=[]
    if mutation=='source-before':(m.SELFHOST/'sema/a_instantiate.wz').write_text('changed')
    def process(*args):
     called.append(True)
     paths={'source-during':m.SELFHOST/'sema/a_instantiate.wz','stdlib-during':m.STDLIB/'text.wz','driver-during':work/'component/src/main.wz','fixture-during':work/'fixtures/main.wz','stage0-during':Path(report['stage0']),'program-during':program,'new-source-during':m.SELFHOST/'new.wz'}
     if mutation in paths:paths[mutation].write_bytes(b'changed')
     return dict(VALID)
    m._run_process_impl=process
    with self.assertRaises(RuntimeError):m.run([program],'mutation',work,{},1,report)
    self.assertEqual(json.loads((work/'report.json').read_text())['status'],'failed');self.assertEqual(bool(called),mutation!='source-before')
 def test_main_setup_failure_has_running_then_failed_report(self):
  work,program,report=setup('main-failure');stage0=Path(report['stage0']);parent=work/'new-runs'
  original_args=m.parse_args;original_stage=m.stage_project
  m.parse_args=lambda:argparse.Namespace(woozyc=str(stage0),reference='',work_dir=str(parent),timeout=1)
  def stage(work):
   self.assertEqual(json.loads((work/'report.json').read_text())['status'],'running');raise RuntimeError('deliberate setup failure')
  m.stage_project=stage
  try:
   with self.assertRaises(RuntimeError):m.main()
   paths=list(parent.glob('sema-recursion-*/report.json'));self.assertEqual(len(paths),1);d=json.loads(paths[0].read_text());self.assertEqual(d['status'],'failed');self.assertIn('deliberate setup failure',d['error'])
  finally:m.parse_args=original_args;m.stage_project=original_stage

if __name__=='__main__':unittest.main(verbosity=2)
