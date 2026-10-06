"""Prepare a closed-panel desk as a parameter-driven prop on the correct author.

This appends prop textures and meshes without changing any existing keyforms.
The optional complete-hands mode binds one whole generated right arm and hand
to the original parent and typing grid, with the left hand hidden behind the laptop.
Run edit_live2d_author.py with the resulting plan in a fresh output directory.
"""
import argparse
import hashlib
import json
import itertools
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / 'art/live2d/desk-work-20261006-r1'
CMO_SHA = 'f7ee66b4ed98ff48510ef777c59f83cf62b0b081c4a94fcff11952409888dce5'
MOC_SHA = 'fbc35062526efe778d41bb1b727841dff30c0fd0c08b74efec6b254072e65a4d'
ATLAS_SHA = 'e27d3aac6faa2eb0cbfc8e3275ccc57ec563aeaa685cd53a4a0cd5151ad6b90e'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def silhouette_quad(texture, target, uniform_scale=False):
    """Register visible alpha, retaining every pixel of the source page in UV."""
    with Image.open(texture) as image:
        if image.mode != 'RGBA':
            raise ValueError(f'The complete material needs generated alpha: {texture}')
        width, height = image.size
        yy, xx = np.where(np.asarray(image.getchannel('A')) >= 16)
        if not len(xx):
            raise ValueError(f'Empty complete material: {texture}')
        box = [int(xx.min()), int(yy.min()), int(xx.max()+1), int(yy.max()+1)]
    left, top, right, bottom = target
    if right <= left or bottom <= top:
        raise ValueError(f'Invalid silhouette target: {target}')
    sx, sy = (right-left)/(box[2]-box[0]), (bottom-top)/(box[3]-box[1])
    if uniform_scale:
        # Right-arm placement is defined by its left/top and visible height.
        # Compute the right edge from source alpha, preserving its proportions.
        sx = sy
        right = left+(box[2]-box[0])*sx
    x, y = left-box[0]*sx, top-box[1]*sy
    return np.asarray([[x,y], [x+width*sx,y], [x,y+height*sy],
                       [x+width*sx,y+height*sy]]), box, [left,top,right,bottom], [sx,sy]


def bind_complete_quad(rig, texture, target, identifier, template, page, order, visibility,
                       uniform_scale=False):
    """Fit each original parent-local keyform to a whole material quad."""
    old = rig['public_drawable']
    rest = np.asarray(old['getMesh']['getPositions'],float).reshape(-1,2)
    native = np.asarray(rig['native_default_source1254'],float)
    native_basis = np.column_stack((native,np.ones(len(native))))
    world_to_parent = np.linalg.lstsq(native_basis,rest,rcond=None)[0]
    registration_residual = float(abs(native_basis@world_to_parent-rest).max())
    world, alpha_box, actual_target, scale = silhouette_quad(texture,target,uniform_scale)
    quad_rest = np.column_stack((world,np.ones(4)))@world_to_parent
    grid = old['getGeometryGrid']
    axes = [{'parameter':a['getParameterId-WD9NFvw'],'keys':a['getKeys']} for a in grid['getAxes']]
    rest_basis = np.column_stack((rest,np.ones(len(rest))))
    forms, residuals = [], []
    for cell in grid['getCells']:
        posed = rest+np.asarray(cell['getForm']['getPositionDeltas']).reshape(-1,2)
        motion_map = np.linalg.lstsq(rest_basis,posed,rcond=None)[0]
        residuals.append({'coordinate':cell['getCoordinate'],
                          'maxResidual':float(abs(rest_basis@motion_map-posed).max())})
        posed_quad = np.column_stack((quad_rest,np.ones(4)))@motion_map
        delta = (posed_quad-quad_rest).astype(np.float32).reshape(-1).tolist()
        for desk in range(2):
            forms.append({'coordinate':cell['getCoordinate']+[desk],'position_deltas':delta})
    maximum_residual = max(row['maxResidual'] for row in residuals)
    # Parent-local coordinates are normalized around the 1254px author canvas.
    # Larger non-affine source motion is recorded for review, never concealed.
    affine_review_threshold = .001
    contract = {'full_silhouette_source_bbox':actual_target,
                'requested_silhouette_source_bbox':list(target),'png_alpha_bbox':alpha_box,
                'uniform_scale':uniform_scale,'silhouette_scale':scale,
                'texture_world_quad':world.tolist(),'parent':old['getParentDeformerId-lmpY1tE'],
                'registration_affine_max_residual':registration_residual,
                'parent_affine_max_residual':maximum_residual,
                'parent_affine_cell_residuals':residuals,
                'affine_review_threshold':affine_review_threshold,
                'non_affine_review_required':maximum_residual > affine_review_threshold,
                'source_grid_cells':len(grid['getCells']),
                'geometry_axes':[row['parameter'] for row in axes]}
    mesh = {'id':identifier,'name':'complete workstation material '+identifier,
        'template_id':template,'parent_id':old['getParentDeformerId-lmpY1tE'],
        'part_id':'PartExtra','texture_page':page,'positions':quad_rest.astype(np.float32).reshape(-1).tolist(),
        'uvs':[0,0,1,0,0,1,1,1],'indices':[0,1,2,2,1,3],
        'axes':axes+[{'parameter':'ParamDeskVisible','keys':[0,1]}],'keyforms':forms,
        'channels':[{'channel':'OPACITY','initial_value':0,'append_axes':[
            {'parameter':'ParamDeskVisible','keys':[0,1]}, {'parameter':visibility,'keys':[0,1]}],
            'keyforms':[{'coordinate':[desk,visible],'value':desk*visible}
                        for desk,visible in itertools.product(range(2),repeat=2)]}],
        'draw_order':order,'opacity':0}
    return mesh, contract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-moc', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--top', type=float, default=850.)
    parser.add_argument('--complete-hands', action='store_true',
                        help='Use the whole right arm page and navy laptop; hide the left hand.')
    parser.add_argument('--sleeve-r-bbox', type=float, nargs=4, default=[700,606,815,796],
                        metavar=('LEFT','TOP','RIGHT','BOTTOM'),
                        help='Right-arm alpha left/top and height; actual right is computed with uniform scale.')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise ValueError('Use a fresh plan output; keep earlier revisions.')
    cmo = ROOT / 'art/live2d/whale-girl-layered-draft.cmo3'
    atlas = ROOT / 'assets/live2d/whale-girl/whale-girl-layered-draft.4096/texture_00.png'
    moc = args.reference_moc.resolve(strict=True)
    for path, expected in [(cmo, CMO_SHA), (moc, MOC_SHA), (atlas, ATLAS_SHA)]:
        if sha(path) != expected:
            raise ValueError(f'Correct formal-equivalent source required: {path}')
    texture = PACK / 'desk-closed-panel.png'
    with Image.open(texture) as image:
        if image.mode != 'RGBA':
            raise ValueError('The desk needs its genuine generated alpha.')
        width, height = image.size
        alpha = np.asarray(image.getchannel('A'))
        yy, xx = np.where(alpha >= 250)
        box = [int(xx.min()), int(yy.min()), int(xx.max()+1), int(yy.max()+1)]
    # The opaque panel reaches the historical standing floor. Frame padding is
    # retained in UV, with the four parent-local corners registered to its alpha.
    left, right, bottom = 70., 1185., 1237.516693532467
    scale_x = (right-left) / (box[2]-box[0])
    # The rear top stays behind hands/computer. Its front edge and closed panel
    # are in front, hiding the old material's cloth fringe below the computer.
    split_y, front_y = 290., 954.
    x0, x1 = left-box[0]*scale_x, left+(width-box[0])*scale_x
    top_y = args.top-box[1]*(front_y-args.top)/(split_y-box[1])
    bottom_y = front_y+(height-split_y)*(bottom-front_y)/(box[3]-split_y)
    pieces = [
        ('ArtMeshWorkstationDeskTop','workstation rear tabletop',31,0.,split_y,top_y,front_y),
        ('ArtMeshWorkstationDesk','workstation front edge and closed panel',35,split_y,float(height),front_y,bottom_y)
    ]
    # Verified default root Shift warp is affine. These are public CMO parent
    # coordinates; runtime Core vertices are not substituted into this chart.
    origin = np.asarray([31.238708, -67.263])
    extent = np.asarray([1247.0283, 1317.263])-origin
    hidden_delta = np.tile([1280./extent[0], 0.], 4).tolist()
    meshes, world_quads = [], []
    for identifier,name,order,uv0,uv1,y0,y1 in pieces:
        world = np.asarray([[x0,y0],[x1,y0],[x0,y1],[x1,y1]])
        world_quads.append(world.tolist())
        local = (world-origin)/extent
        meshes.append({
            'id':identifier,'name':name,
            'template_id':'ArtMeshBottomwear','parent_id':'DeformBodyShift',
            'part_id':'PartExtra','texture_page':1,
            'positions':local.astype(np.float32).reshape(-1).tolist(),
            'uvs':[0,uv0/height,1,uv0/height,0,uv1/height,1,uv1/height],
            'indices':[0,1,2,2,1,3],
            'axes':[{'parameter':'ParamDeskVisible','keys':[0,.001,1]}],
            'keyforms':[{'coordinate':[0],'position_deltas':hidden_delta},
                        {'coordinate':[1],'position_deltas':(np.asarray(hidden_delta)*.999).tolist()},
                        {'coordinate':[2],'position_deltas':[0.]*8}],
            'channels':[{'channel':'OPACITY','initial_value':0,
                         'append_axes':[{'parameter':'ParamDeskVisible','keys':[0,.001,1]}],
                         'keyforms':[{'coordinate':[0],'value':0},
                                     {'coordinate':[1],'value':1},
                                     {'coordinate':[2],'value':1}]}],
            'draw_order':order,'opacity':0
        })
    laptop = PACK/('laptop-uniform-navy.png' if args.complete_hands else 'laptop-complete.png')
    rig_path = PACK/'source-laptop-rig.json'
    rig = json.loads(rig_path.read_text(encoding='utf8'))
    assert rig['source_author_sha256'] == CMO_SHA
    # Preserve the entire generated shell. Register the texture's padding around
    # its complete silhouette; the table front edge is below its lower rim.
    laptop_mesh, laptop_contract = bind_complete_quad(rig,laptop,[344,641,756,821],
        'ArtMeshWorkstationLaptop','ArtMeshObjects',2,32,'ParamLaptopVisible')
    laptop_mesh['name'] = 'one rigid workstation laptop chassis and keyboard'
    append_pages = [{'path':str(texture),'sha256':sha(texture)}, {'path':str(laptop),'sha256':sha(laptop)}]
    sleeve_contracts = {}
    if args.complete_hands:
        sleeves_path = PACK/'source-sleeve-rig.json'
        sleeves = json.loads(sleeves_path.read_text(encoding='utf8'))
        assert sleeves['source_author_sha256'] == CMO_SHA
        assert sleeves['source_moc_sha256'] == MOC_SHA
        assert sleeves['public_graph_sha256'] == rig['public_graph_sha256']
        sleeve = PACK/'complete-arm-r-r2.png'
        mesh, contract = bind_complete_quad(sleeves['sleeves']['R'],sleeve,args.sleeve_r_bbox,
            'ArtMeshWorkstationSleeveR','ArtMeshObjects2',3,33,'ParamBusyLaptop',uniform_scale=True)
        mesh['name'] = 'whole workstation right arm, sleeve, and hand'
        meshes.append(mesh)
        append_pages.append({'path':str(sleeve),'sha256':sha(sleeve)})
        sleeve_contracts['R'] = contract
    meshes.append(laptop_mesh)
    plan = {
        'schema_version': 2,
        'kind': 'isolated_closed_panel_workstation',
        'description': 'Closed opaque desk hides lower body; existing character remains live parameter animation.',
        'source': {'cmo_sha256': CMO_SHA, 'moc_sha256': MOC_SHA,
                   'atlas_pages': [{'path':str(atlas),'sha256':ATLAS_SHA}]},
        'parameters': [{'id':'ParamDeskVisible','name':'小工位滑入 / 收回',
                        'min':0,'max':1,'default':0}],
        'append_pages': append_pages,
        'insert_meshes': meshes,
        'coordinate_contract': {
            'parent':'DeformBodyShift', 'root_default_origin':origin.tolist(),
            'root_default_extent':extent.tolist(), 'png_opaque_bbox':box,
            'opaque_source_target':[left,args.top,right,bottom],
            'texture_split_y':split_y,'front_edge_source_y':front_y,
            'texture_world_quads':world_quads,
            'laptop_full_silhouette_source_bbox':[344,641,756,821],
            'keyboard_owner':'ArtMeshWorkstationLaptop; same vertices and transform as chassis; no typing axes',
            'laptop_source_rig_sha256':sha(rig_path),
            'laptop_parent_affine_max_residual':laptop_contract['parent_affine_max_residual'],
            'floor_owner':'runtime desk counter-translation cancels character-only grounding',
            'transition':'cover fully first; body recovery first on exit; desk retracts last'
        },
        'provenance': {'producer':'tools/prepare_desk_work.py',
                       'producer_sha256':sha(Path(__file__)),
                       'status':'pending_visual_review','adopted':False,
                       'crouch_source_used':False}
    }
    if args.complete_hands:
        plan['coordinate_contract'].update({
            'complete_hands':True,'sleeve_source_rig_sha256':sha(sleeves_path),
            'sleeves':sleeve_contracts,'sleeve_visibility':'ParamDeskVisible * ParamBusyLaptop',
            'hiddenLeftHand':True,
            'replacement_sleeves':{'ArtMeshObjects2':'ArtMeshWorkstationSleeveR'},
            'render_order_contract':'deskTop < completeLaptop < completeSleeveR < deskFront'})
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(plan,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    print(json.dumps({'plan':str(output),'sha256':sha(output),'world_quads':world_quads,
                      'sleeve_affine_max_residuals':{side:row['parent_affine_max_residual']
                                                    for side,row in sleeve_contracts.items()},
                      'non_affine_review_required':any(row['non_affine_review_required']
                                                      for row in sleeve_contracts.values())}))


if __name__ == '__main__':
    main()
