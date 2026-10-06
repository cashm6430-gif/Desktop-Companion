"""Isolated public-grid geometry helpers with actual hierarchical warp domains.

This is not a model exporter. Child control points are evaluated by their
parent world before a drawable is evaluated in its immediate parent's world.
All grids/positions/UVs originate from the frozen, Core-equivalent author graph.
"""
import numpy as np
import generate_first_geometry as source

BODY_KEYS = [-10.0, 0.0, 10.0]
SIT_KEYS = source.KEYS
BASE_PLAN = source.read(source.OUT/'candidate-grid-plan.json')
BODY_EDITS = {tuple(c['coordinate']):np.asarray(c['control_points']).reshape(-1,2)
              for c in BASE_PLAN['deformer_grids'][0]['keyforms']}


def weights(keys, value):
    if value <= keys[0]:return [(0,1.0)]
    if value >= keys[-1]:return [(len(keys)-1,1.0)]
    upper=int(np.searchsorted(keys,value));lower=upper-1
    t=(value-keys[lower])/(keys[upper]-keys[lower])
    return [(lower,1-t),(upper,t)]


def interpolate_grid(node, params, authored=False):
    grid=node['getGeometryGrid']
    cells={tuple(c['getCoordinate']):np.asarray(c['getForm']['getControlPoints']).reshape(-1,2)
           for c in grid['getCells']}
    axes=[(a['getParameterId-WD9NFvw'],a['getKeys']) for a in grid['getAxes']]
    if authored and node['getId-u_t8HeU']=='DeformBodyXY':
        cells={(coord+(0,)):cp for coord,cp in cells.items()}
        cells.update(BODY_EDITS)
        axes.append(('ParamSitPose',SIT_KEYS))
    selected=[([],1.0)]
    for name,keys in axes:
        selected=[(coord+[index],weight*coefficient)
                  for coord,weight in selected
                  for index,coefficient in weights(keys,params.get(name,source.default[name]))]
    cp=sum((weight*cells[tuple(coord)] for coord,weight in selected),np.zeros_like(next(iter(cells.values()))))
    return cp


def warp_apply(node, lattice, points):
    columns,rows=node['getColumns'],node['getRows']
    cp=np.asarray(lattice).reshape(rows+1,columns+1,2)
    points=np.asarray(points,dtype=float).reshape(-1,2)
    scaled=points*[columns,rows]
    cell=np.clip(np.floor(scaled).astype(int),[0,0],[columns-1,rows-1])
    u,v=(scaled-cell).T;x,y=cell.T
    return (cp[y,x]*(1-u[:,None])*(1-v[:,None])
            +cp[y,x+1]*u[:,None]*(1-v[:,None])
            +cp[y+1,x]*(1-u[:,None])*v[:,None]
            +cp[y+1,x+1]*u[:,None]*v[:,None])


def world_lattices(params, authored=False):
    cache={}
    def resolve(identifier):
        if identifier in cache:return cache[identifier]
        node=source.W[identifier];cp=interpolate_grid(node,params,authored)
        parent=node['getParent-lmpY1tE']
        if parent:cp=warp_apply(source.W[parent],resolve(parent),cp)
        cache[identifier]=cp
        return cp
    return resolve


def warp_inverse(node,lattice,world):
    """Invert the proven monotone, Y-separable actual final warp lattice.

    This is piecewise exact, including extended edge cells. It does not fit
    a global affine matrix to a bent BodyXY lattice.
    """
    columns,rows=node['getColumns'],node['getRows']
    cp=np.asarray(lattice).reshape(rows+1,columns+1,2)
    world=np.asarray(world,dtype=float).reshape(-1,2)
    assert np.max(np.ptp(cp[:,:,1],axis=1))<1e-5,'Y must be independent of local X'
    ys=cp[:,0,1]
    assert np.all(np.diff(ys)>0),'Y axis must be monotone'
    yi=np.clip(np.searchsorted(ys,world[:,1],side='right')-1,0,rows-1)
    v=(world[:,1]-ys[yi])/(ys[yi+1]-ys[yi])
    row=cp[yi,:,0]*(1-v[:,None])+cp[yi+1,:,0]*v[:,None]
    assert np.all(np.diff(row,axis=1)>0),'X axis must be monotone'
    xi=np.minimum(np.maximum(np.sum(world[:,0,None]>=row,axis=1)-1,0),columns-1)
    i=np.arange(len(world));u=(world[:,0]-row[i,xi])/(row[i,xi+1]-row[i,xi])
    return np.column_stack(((xi+u)/columns,(yi+v)/rows))


def mesh_world(node,params,authored=False,local=None):
    if local is None:local=source.absolute_form(node,source.neutral_cell(node)).astype(float)
    parent=node['getParentDeformerId-lmpY1tE']
    return warp_apply(source.W[parent],world_lattices(params,authored)(parent),local)


def local_from_world(node,params,world,authored=True):
    parent=node['getParentDeformerId-lmpY1tE']
    return warp_inverse(source.W[parent],world_lattices(params,authored)(parent),world)


def context(xindex,yindex,sit=0):
    return {'ParamBodyAngleX':BODY_KEYS[xindex],'ParamBodyAngleY':BODY_KEYS[yindex],
            'ParamSitPose':sit}


def key_settings(sit):
    return [sum(weight*values[index] for index,weight in weights(SIT_KEYS,sit))
            for values in [source.DROPS,source.BENDS,source.MIN_WIDTHS]]


def local_jacobian_bound(node,lattice):
    cols,rows=node['getColumns'],node['getRows']
    cp=np.asarray(lattice).reshape(rows+1,cols+1,2)
    # Y-separable bilinear cells have det=dX/dx*dY/dy. dX/dx is
    # affine in local y, hence its cell minimum is on the row endpoints.
    assert np.max(np.ptp(cp[:,:,1],axis=1))<1e-5
    dx=np.diff(cp[:,:,0],axis=1)*cols
    dy=np.diff(cp[:,0,1])*rows
    return float(min((min(dx[y,x],dx[y+1,x])*dy[y]
                      for y in range(rows) for x in range(cols))))
