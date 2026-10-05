"""A passing material check cannot bypass failed/stale author/Core evidence."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import review_component_candidate as tool


class CandidateReviewTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.directory=Path(self.tmp.name)
        self.moc=self.directory/'edited.moc3';self.moc.write_bytes(b'fake MOC; Native component checks are mocked here')
        self.png=self.directory/'texture_00.png';Image.new('RGBA',(2,2),(80,80,110,255)).save(self.png)
        self.model=self.directory/'edited.model3.json';self.save(self.model,{'Version':3,'FileReferences':{'Moc':self.moc.name,'Textures':[self.png.name]}})
        self.metadata=self.directory/'edited.psd2live.json';self.save(self.metadata,{})
        self.fixture=self.directory/'fixture.json';self.save(self.fixture,{'poses':[]})
        self.edit=self.directory/'edit-report.json';self.save(self.edit,{'kind':tool.EDIT_KIND,**{field:True for field in tool.EDIT_BOOLEANS},
            'output_sha256':{'edited.moc3':tool.digest(self.moc)},
            'cmo_exact_embedded_png_pages':[{'page':0,'input_png_bytes_exact':True,'png_sha256':tool.digest(self.png)}]})
        self.dense=self.directory/'dense-report.json';self.save(self.dense,{'kind':tool.DENSE_KIND,'engineering_passed_within_scope':True,
            'inputs':{str(self.moc):{'sha256':tool.digest(self.moc)}}})
        self.coverage_pass=True;self.change_metadata=False

    def save(self,path,data):path.write_text(json.dumps(data),encoding='utf8')

    def inventory(self,model,output,metadata=None):
        document={'kind':'inventory','structural_passed':True,'inputs':{str(self.metadata):tool.digest(self.metadata)}}
        self.save(output,document);return document

    def coverage(self,model,fixture,output,metadata=None):
        if self.change_metadata:self.save(self.metadata,{'changed_during_review':True})
        document={'kind':'coverage','passed':self.coverage_pass,'inputs':{str(self.metadata):tool.digest(self.metadata)}}
        self.save(output,document);return document

    def review(self):
        with patch.object(tool,'run_inventory',side_effect=self.inventory),patch.object(tool,'run_coverage',side_effect=self.coverage):
            return tool.review(self.model,self.metadata,self.fixture,self.edit,self.dense,self.directory/'gate.json')

    def test_failed_dense_gate_blocks_even_passing_material_checks(self):
        dense=tool.read(self.dense);dense['engineering_passed_within_scope']=False;self.save(self.dense,dense)
        gate=self.review()
        self.assertFalse(gate['engineering_passed_within_scope']);self.assertEqual(gate['adoption'],'not_adopted')
        self.assertFalse(gate['adopted'])
        self.assertTrue(next(s for s in gate['stages'] if s['id']=='named_joint_material_coverage')['passed'])

    def test_stale_author_or_dense_MOC_sha_cannot_pass(self):
        for field in ['author','dense']:
            with self.subTest(field=field):
                path=self.edit if field=='author' else self.dense;old=path.read_bytes();document=tool.read(path)
                if field=='author':document['output_sha256']['edited.moc3']='0'*64
                else:document['inputs'][str(self.moc)]['sha256']='0'*64
                self.save(path,document)
                with patch.object(tool,'run_inventory',side_effect=self.inventory),patch.object(tool,'run_coverage',side_effect=self.coverage):
                    gate=tool.review(self.model,self.metadata,self.fixture,self.edit,self.dense,self.directory/(field+'-gate.json'))
                self.assertFalse(gate['engineering_passed_within_scope']);self.assertEqual(gate['status'],'blocked_engineering')
                path.write_bytes(old)

    def test_false_roundtrip_and_failed_coverage_cannot_pass(self):
        edit=tool.read(self.edit);edit['direct_vs_saved_CMO_readback_all_runtime_values_exact']=False;self.save(self.edit,edit)
        self.coverage_pass=False;gate=self.review()
        failed={stage['id'] for stage in gate['stages'] if not stage['passed']}
        self.assertIn('public_author_roundtrip',failed);self.assertIn('named_joint_material_coverage',failed)

    def test_mid_review_input_change_is_not_hidden_by_newer_component_pins(self):
        self.change_metadata=True;gate=self.review()
        stage=next(s for s in gate['stages'] if s['id']=='inputs_stable_after_review')
        self.assertFalse(stage['passed']);self.assertTrue(stage['cross_stage_pin_conflicts'])
        self.assertFalse(gate['engineering_passed_within_scope'])

    def test_new_hand_trajectory_cannot_reuse_old_half_opacity_fixture(self):
        old=self.directory/'approved-plan.json';new=self.directory/'current-plan.json'
        self.save(old,{'insert_meshes':[{'id':'ArtMeshGroundForearmL','axes':[{'parameter':'ParamSitPose','keys':[0,.35,.65,1]}]}]})
        self.save(new,{'insert_meshes':[{'id':'ArtMeshGroundForearmL','axes':[{'parameter':'ParamSitPose','keys':[0,.32,.36,.65,.96,1]}]}]})
        self.save(self.fixture,{'poses':[],'expected_topology':{'ArtMeshGroundForearmL':{'vertices':3,'triangles':1}},'inputs':{str(old):tool.digest(old)}})
        edit=tool.read(self.edit);edit['input_sha256']={str(new):tool.digest(new)};self.save(self.edit,edit)
        gate=self.review();stage=next(s for s in gate['stages'] if s['id']=='fixture_author_motion_binding')
        self.assertFalse(stage['passed']);self.assertIn('regenerate',stage['reasons'][0])
        self.assertFalse(gate['engineering_passed_within_scope'])

    def test_engineering_pass_still_requires_visual_and_never_adopts_or_overwrites(self):
        gate=self.review();self.assertTrue(gate['engineering_passed_within_scope'])
        self.assertFalse(gate['adopted']);self.assertFalse(gate['automatic_adoption'])
        self.assertEqual(gate['visual_review']['user_visual_approval'],'pending')
        pin=tool.digest(self.directory/'gate.json')
        with self.assertRaisesRegex(ValueError,'fresh'):self.review()
        self.assertEqual(tool.digest(self.directory/'gate.json'),pin)


if __name__=='__main__':unittest.main()
