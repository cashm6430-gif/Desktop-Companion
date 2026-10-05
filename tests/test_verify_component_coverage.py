"""Coverage checks must reject transparent material and hidden Parts."""
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from verify_component_coverage import (CoverageModel,clipped_alpha,
    fixture_points,mesh_texture_alpha,sample_texture_alpha)
from verify_component_asset_inventory import chart_inventory


class MaterialCoverageTests(unittest.TestCase):
    def test_real_png_transparency_and_native_v(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'alpha.png'
            rgba=np.zeros((8,8,4),dtype=np.uint8)
            rgba[:4,:4]=[200,100,80,255]
            Image.fromarray(rgba).save(path)
            alpha=np.asarray(Image.open(path))[:,:,3]/255
            sampled=sample_texture_alpha(alpha,[[.125,.875],[.875,.875],[.125,.125]])
            np.testing.assert_allclose(sampled,[1,0,0])

    def test_triangle_rectangle_is_not_material_coverage(self):
        values=mesh_texture_alpha([[.2,.2],[.8,.8]],[[0,0],[1,0],[0,1]],
                                  [[0,0],[1,0],[0,1]],[[0,1,2]],np.ones((8,8)))
        np.testing.assert_allclose(values,[1,0])

    def test_opacity_and_inverted_clip(self):
        np.testing.assert_allclose(clipped_alpha([1],.5,np.asarray([.25])),[.125])
        np.testing.assert_allclose(clipped_alpha([1],.5,np.asarray([.25]),True),[.375])

    def test_full_coverage_inverted_mask_ignores_mask_drawable_opacity(self):
        runtime=CoverageModel.__new__(CoverageModel)
        runtime.lookup={'body':0,'clip':1};runtime.opacity={'body':.5,'clip':0}
        runtime.visible={'body':True,'clip':False}
        runtime.topology={'body':{'masks':['clip'],'inverted_mask':True}}
        runtime.raw_alpha=lambda name,points:np.full(len(points),1.0 if name=='body' else .25)
        alpha,_=runtime.coverage(['body'],[[0,0]])
        np.testing.assert_allclose(alpha,[.375])

    def test_joint_roi_follows_actual_rotated_anchor(self):
        class Joint:
            ppu=1.;origin=np.asarray([0.,0.])
            def source_xy(self,p):return np.asarray(p)*[1,-1]
            def anchor(self,d):return np.asarray(d['source_xy'])*[1,-1]
        check={'anchor':{'source_xy':[0,0]},'offset_frame':{'toward_anchor':{'source_xy':[10,0]}},
               'offsets_source_pixels':[[0,4],[2,0]]}
        _,offsets,points=fixture_points(Joint(),check)
        np.testing.assert_allclose(offsets,[[4,0],[0,-2]])
        np.testing.assert_allclose(Joint().source_xy(points),offsets)

    def test_uv_inventory_reports_real_triangle_alpha_and_texture_edges(self):
        alpha=np.ones((8,8));alpha[4:,:]=0
        stats=chart_inventory([[0,1],[1,1],[0,0]],[[0,1,2]],alpha)
        self.assertGreater(stats['transparent_texels_in_uv_triangles'],0)
        self.assertGreater(stats['opaque_texels_in_chart_rectangle_outside_uv_triangles'],0)
        self.assertIn('left',stats['opaque_texture_page_edges_touched'])
        self.assertIn('top',stats['opaque_texture_page_edges_touched'])
        self.assertNotIn('bottom',stats['opaque_texture_page_edges_touched'])
        outside=chart_inventory([[-.1,1],[1,1],[0,0]],[[0,1,2]],alpha)
        self.assertEqual(outside['uv_vertices_outside_0_1'],1)

    def test_actual_native_part_opacity_is_evaluated_once(self):
        model=ROOT/'assets/live2d/whale-girl/whale-girl-layered-draft.model3.json'
        runtime=CoverageModel(model)
        results=[]
        for opacity in [1,.5,0]:
            runtime.update({'name':'part','parameters':{},'part_opacities':{'PartBody':opacity}})
            point=runtime.anchor({'source_xy':[570,1200]})
            alpha,_=runtime.coverage(['ArtMeshFootwearL'],[point])
            results.append(float(alpha[0]))
        self.assertGreater(results[0],.5)
        self.assertAlmostEqual(results[1],results[0]*.5,places=7)
        self.assertEqual(results[2],0)


if __name__=='__main__':unittest.main()
