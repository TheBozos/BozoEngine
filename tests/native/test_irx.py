"""Strict native IRX generator checks using synthetic instructions only."""
import importlib.util,json,pathlib,struct,subprocess,sys,tempfile,unittest
root=pathlib.Path(__file__).resolve().parents[2]
script=root/'tools/native_irx/recompile.py'
spec=importlib.util.spec_from_file_location('irx_recompile',script)
tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
class StrictNative(unittest.TestCase):
    def fixture(self,folder,words):
        path=pathlib.Path(folder)/'fixture.json'
        path.with_suffix('.bin').write_bytes(struct.pack('<'+'I'*len(words),*words))
        path.write_text(json.dumps(dict(name='fixture.irx',base=0x1000,size=4*len(words),entry=0x1000,gp=0,hash='1234')))
        return path
    def test_unsupported_is_an_explicit_native_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            path=self.fixture(temp,[0xffffffff])
            result=subprocess.run([sys.executable,'-I',str(script),str(path),'--out',temp],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            output=(path.parent/'fixture_iop.cpp').read_text()
            self.assertIn('throw std::runtime_error',output)
            self.assertIn('unsupported instruction 0xffffffff',output)
    def test_native_return_and_no_compatibility_mode(self):
        with tempfile.TemporaryDirectory() as temp:
            path=self.fixture(temp,[0x03e00008,0])
            module=tool.Module(path)
            exports,imports=module.tables()
            functions=tool.find_functions(module,exports,imports,[])
            emitted=tool.emit_module(module,functions)
            self.assertIn('bozo/native/compiled_irx.h',emitted)
            self.assertIn('throw std::runtime_error',emitted)
            with self.assertRaises(ValueError):tool.emit_module(module,functions,False)
    def test_load_delay_cannot_fall_back(self):
        with tempfile.TemporaryDirectory() as temp:
            path=self.fixture(temp,[0x8c820000,0x00401821,0x03e00008,0])
            result=subprocess.run([sys.executable,'-I',str(script),str(path),'--out',temp],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
    def test_switch_case_decoded_before_the_jump_is_an_entry(self):
        with tempfile.TemporaryDirectory() as temp:
            path=self.fixture(temp,[0x10800003,0,0x03e00008,0,0x01000008,0,0x00001008])
            module=tool.Module(path)
            exports,imports=module.tables()
            functions=tool.find_functions(module,exports,imports,[])
            self.assertIn(0x1008,functions[0x1000].resume)
if __name__=='__main__':unittest.main()
