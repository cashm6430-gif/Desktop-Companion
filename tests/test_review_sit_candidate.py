"""Authorized seated changes must never exempt standing or controller drift."""
from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
from PIL import Image

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import review_sit_candidate as tool


class SitCandidateReviewTests(unittest.TestCase):
    def compare(self,change_standing=False,change_trace=False):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);receipts=[]
            for label in ['formal','candidate']:
                folder=root/label;(folder/'static').mkdir(parents=True)
                (folder/'capture-receipt.json').write_text('{}',encoding='utf8')
                for i in range(2):
                    changed=label=='candidate' and (i==1 or change_standing)
                    Image.new('RGBA',(2,2),(0,0,100 if changed else 0,255)).save(folder/'static'/f'pose-{i:02}.png')
                traces=[{'parameters':{'ParamSitPose':i}} for i in range(2)]
                if label=='candidate' and change_trace:traces[1]['parameters']['ParamBusyLaptop']=1
                receipts.append((folder,{'shared_inputs':[],'scope':'static','fixture':'same','raw_export_only':False,
                    'sequences':[{'kind':'static','name':'fixture','folder':'static','frames':['pose-00.png','pose-01.png'],'trace':traces}]}))
            with patch.object(tool,'load_capture',side_effect=receipts):return tool.compare(root/'formal',root/'candidate',root/'report.json')

    def test_seated_pixel_change_keeps_standing_exact(self):
        result=self.compare();self.assertTrue(result['engineering_passed_within_scope'])
        self.assertEqual(result['unchanged_standing_frames'],1);self.assertEqual(result['changed_sitting_frames'],1)
        self.assertFalse(result['adopted'])

    def test_standing_pixel_or_controller_trace_change_blocks(self):
        self.assertFalse(self.compare(change_standing=True)['engineering_passed_within_scope'])
        self.assertFalse(self.compare(change_trace=True)['engineering_passed_within_scope'])


if __name__=='__main__':unittest.main()
