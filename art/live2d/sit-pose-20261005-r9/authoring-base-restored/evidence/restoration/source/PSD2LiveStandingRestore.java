import org.umamo.format.moc3.Moc3;
import org.umamo.format.moc3.MocDocument;
import org.umamo.format.cmo3.Cmo3;
import org.umamo.format.cmo3.model.custom.CModelSource;
import org.umamo.format.cmo3.model.custom.CImageResource;
import org.umamo.format.cmo3.model.gen.*;
import org.umamo.format.cmo3.model.identity.Guid;
import org.umamo.format.cmo3.model.identity.Id;
import org.umamo.interop.cmo3.Cmo3Conversion;
import org.umamo.interop.moc3.Moc3ExportOptions;
import org.umamo.interop.moc3.export.Moc3Export;
import org.umamo.runtime.model.*;
import io.github.psd2live.core.Cmo3ModelImport;
import kotlinx.serialization.json.*;
import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

/**
 * Version-pinned standalone public CMO parent-local editor. No GUI, VM, MCP,
 * private reflection, source PSD rebinding, global coordinate conversion, or
 * production adoption. Required option/value pairs: --install --cmo
 * --reference-moc --edit-json --output --store. --atlas is required only for schema1. Output/store must be new
 * isolated paths; -Dpsd2live.agent.store must point to --store before JVM launch.
 *
 * Edit JSON schema_version=2, source={cmo_sha256,moc_sha256,atlas_pages:[{path,sha256},...]}; ordered pages must match saved author CMO and reference MOC. Schema1 single-page is also accepted.
 * deformer_grids/mesh_grids rows: id, append_axes/replace_axes, keyforms.
 * An axis row has parameter,keys; replace additionally source_key_indices.
 * Forms have coordinate and control_points/position_deltas; mesh may instead
 * supply absolute_parent_positions (intentional new form only). channels rows
 * have channel, append_axes/replace_axes/keyforms with scalar value or rgb.
 * Mesh base, UV, topology, parents, old nonedited grids/channels stay unchanged.
 * Sit0 cells are protected, and new-axis Cartesian copies retain original bits.
 *
 * New parameters: id,name,min,max,default; must be referenced by a real grid.
 * append_pages: absolute path, sha256; original page remains byte-exact.
 * insert_meshes: id,name,template_id,parent_id,part_id,texture_page,positions,
 * uvs,indices,axes,keyforms,channels,draw_order,opacity. Positions/deltas are
 * caller-resolved parent-local coordinates. UVs are PUBLIC PuppetModel UVs.
 * No source-pixel-to-parent inverse is guessed by this editor.
 */
public final class PSD2LiveStandingRestore {
    private static final String JAR_NAME="psd2live-2.0.2-2ad13515cfa82896b5e2819475e01d.jar";
    private static final String JAR_SHA="f1e1663887a6ceefd57dd5cfa25f09b958fb45fbe5a0aa27a72e866bf7b5e3d4";
    private static PuppetModel masterModel;
    private static final Set<String> changedMeshes=new LinkedHashSet<>(),changedDeformers=new LinkedHashSet<>(),newParameters=new LinkedHashSet<>(),insertedMeshes=new LinkedHashSet<>();
    private static final List<Object> operationAudit=new ArrayList<>();
    private static final Map<String,Parameter> parameters=new LinkedHashMap<>();
    private static String sha(Path path)throws Exception{return hash(Files.readAllBytes(path));}
    private static String hash(byte[] bytes)throws Exception{return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));}
    private static String quote(String s){var out=new StringBuilder("\"");for(char c:s.toCharArray()){if(c=='"'||c=='\\')out.append('\\').append(c);else if(c<32)out.append(String.format("\\u%04x",(int)c));else out.append(c);}return out.append('"').toString();}
    private static String json(Object v){if(v==null)return "null";if(v instanceof Number||v instanceof Boolean)return v.toString();if(v instanceof Map<?,?> m){var out=new ArrayList<String>();for(var e:m.entrySet())out.add(quote(e.getKey().toString())+":"+json(e.getValue()));return "{"+String.join(",",out)+"}";}if(v instanceof List<?> l){var out=new ArrayList<String>();for(Object e:l)out.add(json(e));return "["+String.join(",",out)+"]";}return quote(v.toString());}
    private static JsonObject object(JsonElement e){if(!(e instanceof JsonObject o))throw new IllegalArgumentException("JSON object required");return o;}
    private static List<JsonObject> rows(JsonObject o,String key){if(!o.containsKey(key))return List.of();var out=new ArrayList<JsonObject>();for(JsonElement e:(JsonArray)o.get(key))out.add(object(e));return out;}
    private static String string(JsonObject o,String key){if(!(o.get(key) instanceof JsonPrimitive p)||!p.isString())throw new IllegalArgumentException("String required: "+key);return p.getContent();}
    private static float number(JsonObject o,String key){return number(o.get(key));}
    private static float number(JsonElement e){float n=Float.parseFloat(((JsonPrimitive)e).getContent());if(!Float.isFinite(n))throw new IllegalArgumentException("Finite float required");return n;}
    private static int integer(JsonElement e){float v=number(e);if(v!=(int)v)throw new IllegalArgumentException("Integer required");return (int)v;}
    private static float[] floats(JsonElement e){if(!(e instanceof JsonArray a))throw new IllegalArgumentException("Float array required");float[] out=new float[a.size()];for(int i=0;i<out.length;i++)out[i]=number(a.get(i));return out;}
    private static int[] ints(JsonElement e){if(!(e instanceof JsonArray a))throw new IllegalArgumentException("Integer array required");int[] out=new int[a.size()];for(int i=0;i<out.length;i++)out[i]=integer(a.get(i));return out;}
    private static void fields(JsonObject o,String... allowed){Set<String> keys=new HashSet<>(Arrays.asList(allowed));for(String k:o.keySet())if(!keys.contains(k))throw new IllegalArgumentException("Unknown input field: "+k);}
    private static Object valueConstructor(Class<?> type,Object... arguments)throws Exception{Constructor<?> c=Arrays.stream(type.getConstructors()).filter(x->x.getParameterCount()==arguments.length+1&&x.getParameterTypes()[arguments.length]==kotlin.jvm.internal.DefaultConstructorMarker.class).findFirst().orElseThrow();Object[] args=Arrays.copyOf(arguments,arguments.length+1);return c.newInstance(args);}
    private static String id(Object value)throws Exception{return (String)Arrays.stream(value.getClass().getMethods()).filter(m->m.getParameterCount()==0&&m.getName().startsWith("getId-")).findFirst().orElseThrow().invoke(value);}
    private static String axisId(KeyformAxis a)throws Exception{return (String)a.getClass().getMethod("getParameterId-WD9NFvw").invoke(a);}
    private static Object copy(Object value,Map<Integer,Object> changes)throws Exception{Method method=Arrays.stream(value.getClass().getMethods()).filter(m->Modifier.isPublic(m.getModifiers())&&!Modifier.isStatic(m.getModifiers())&&(m.getName().equals("copy")||m.getName().startsWith("copy-"))&&!m.getName().contains("$")).findFirst().orElseThrow();Object[] args=new Object[method.getParameterCount()];for(int i=0;i<args.length;i++){int c=i+1;Method component=Arrays.stream(value.getClass().getMethods()).filter(m->m.getParameterCount()==0&&(m.getName().equals("component"+c)||m.getName().startsWith("component"+c+"-"))).findFirst().orElseThrow();args[i]=changes.containsKey(c)?changes.get(c):component.invoke(value);}return method.invoke(value,args);}
    private static KeyformAxis axis(JsonObject spec)throws Exception{String parameter=string(spec,"parameter");float[] keys=floats(spec.get("keys"));Parameter p=parameters.get(parameter);if(p==null)throw new IllegalArgumentException("Unknown parameter "+parameter);if(keys.length==0)throw new IllegalArgumentException("Empty axis");for(int i=0;i<keys.length;i++)if(keys[i]<p.getMin()||keys[i]>p.getMax()||i>0&&keys[i]<=keys[i-1])throw new IllegalArgumentException("Unsorted/out-of-range keys "+parameter);return (KeyformAxis)valueConstructor(KeyformAxis.class,parameter,keys);}
    private static List<int[]> coordinates(List<KeyformAxis> axes){var out=new ArrayList<int[]>();enumerate(axes,0,new int[axes.size()],out);return out;}
    private static void enumerate(List<KeyformAxis> axes,int at,int[] c,List<int[]> out){if(at==c.length){out.add(c.clone());return;}for(int i=0;i<axes.get(at).getKeys().length;i++){c[at]=i;enumerate(axes,at+1,c,out);}}
    private static Object cloneForm(Object f){if(f instanceof MeshDeltaForm m)return new MeshDeltaForm(m.getPositionDeltas().clone());if(f instanceof WarpLatticeForm w)return new WarpLatticeForm(w.getControlPoints().clone());if(f instanceof RotationPivotForm r)return new RotationPivotForm(r.getOriginX(),r.getOriginY(),r.getAngle(),r.getScale());return f;}
    private static boolean equalForm(Object a,Object b)throws Exception{return graph(a,0).equals(graph(b,0));}
    private static Object replacementForm(Object original,JsonObject spec,float[] base){
        if(original instanceof MeshDeltaForm m){boolean d=spec.containsKey("position_deltas"),p=spec.containsKey("absolute_parent_positions");if(d==p)throw new IllegalArgumentException("Provide exactly one mesh positions field");float[] values=floats(spec.get(d?"position_deltas":"absolute_parent_positions"));if(values.length!=m.getPositionDeltas().length)throw new IllegalArgumentException("Mesh delta length mismatch");if(p)for(int i=0;i<values.length;i++)values[i]-=base[i];return new MeshDeltaForm(values);}
        if(original instanceof WarpLatticeForm w){float[] points=floats(spec.get("control_points"));if(points.length!=w.getControlPoints().length)throw new IllegalArgumentException("Warp CP length mismatch");return new WarpLatticeForm(points);}
        if(original instanceof RotationPivotForm)return new RotationPivotForm(number(spec,"origin_x"),number(spec,"origin_y"),number(spec,"angle"),number(spec,"scale"));
        if(original instanceof ChannelValue.Scalar)return new ChannelValue.Scalar(number(spec,"value"));
        if(original instanceof ChannelValue.Color){float[] rgb=floats(spec.get("rgb"));if(rgb.length!=3)throw new IllegalArgumentException("RGB requires3");for(float v:rgb)if(v<0||v>1)throw new IllegalArgumentException("RGB unit range required");return new ChannelValue.Color(new ColorRgb(rgb[0],rgb[1],rgb[2]));}
        throw new IllegalArgumentException("Unsupported form "+original.getClass());
    }
    @SuppressWarnings("unchecked")
    private static <T> KeyformGrid<T> editGrid(KeyformGrid<T> original,JsonObject spec,float[] base)throws Exception{
        return editGrid(original,spec,base,true);
    }
    @SuppressWarnings("unchecked")
    private static <T> KeyformGrid<T> editGrid(KeyformGrid<T> original,JsonObject spec,float[] base,boolean protectExisting)throws Exception{
        List<KeyformAxis> axes=new ArrayList<>(original.getAxes());Map<String,int[]> remaps=new LinkedHashMap<>();Set<String> touched=new HashSet<>();
        for(JsonObject row:rows(spec,"replace_axes")){fields(row,"parameter","keys","source_key_indices");String name=string(row,"parameter");if(!touched.add(name))throw new IllegalArgumentException("Duplicate axis operation "+name);int index=-1;for(int i=0;i<axes.size();i++)if(axisId(axes.get(i)).equals(name))index=i;if(index<0)throw new IllegalArgumentException("replace_axes missing existing axis "+name);KeyformAxis a=axis(row);int[] mapping=ints(row.get("source_key_indices"));if(mapping.length!=a.getKeys().length)throw new IllegalArgumentException("Axis mapping size mismatch");for(int m:mapping)if(m<0||m>=axes.get(index).getKeys().length)throw new IllegalArgumentException("Bad original axis key index");if(name.equals("ParamSitPose"))for(int i=0;i<a.getKeys().length;i++)if(a.getKeys()[i]==0&&axes.get(index).getKeys()[mapping[i]]!=0)throw new IllegalArgumentException("SitPose0 remap would change protected source cell");remaps.put(name,mapping);axes.set(index,a);}
        for(JsonObject row:rows(spec,"append_axes")){fields(row,"parameter","keys");String name=string(row,"parameter");if(!touched.add(name))throw new IllegalArgumentException("Duplicate axis operation "+name);for(var a:axes)if(axisId(a).equals(name))throw new IllegalArgumentException("append_axes already exists "+name);KeyformAxis added=axis(row);boolean hasDefault=false;for(float key:added.getKeys())if(key==parameters.get(name).getDefault())hasDefault=true;if(!hasDefault)throw new IllegalArgumentException("Appended axis must contain native default key "+name);axes.add(added);}
        Map<String,KeyformCell<T>> source=new LinkedHashMap<>();for(var cell:original.getCells())if(source.put(Arrays.toString(cell.getCoordinate()),cell)!=null)throw new IllegalArgumentException("Duplicate source grid cell");
        if(source.size()!=coordinates(original.getAxes()).size())throw new IllegalArgumentException("Source grid is not complete Cartesian");
        Map<String,KeyformCell<T>> cells=new LinkedHashMap<>();
        for(int[] coord:coordinates(axes)){int[] old=Arrays.copyOf(coord,original.getAxes().size());for(int i=0;i<old.length;i++){int[] map=remaps.get(axisId(original.getAxes().get(i)));if(map!=null)old[i]=map[coord[i]];}var cell=source.get(Arrays.toString(old));if(cell==null)throw new IllegalArgumentException("Missing source Cartesian cell");cells.put(Arrays.toString(coord),new KeyformCell<>(coord,(T)cloneForm(cell.getForm())));}
        Set<String> overrides=new HashSet<>();
        for(JsonObject form:rows(spec,"keyforms")){fields(form,"coordinate","position_deltas","absolute_parent_positions","control_points","origin_x","origin_y","angle","scale","value","rgb");int[] coord=ints(form.get("coordinate"));String key=Arrays.toString(coord);var current=cells.get(key);if(current==null||!overrides.add(key))throw new IllegalArgumentException("Invalid/duplicate target grid coordinate "+key);Object value=replacementForm(current.getForm(),form,base);boolean standing=false,hasSit=false;for(int i=0;i<axes.size();i++)if(axisId(axes.get(i)).equals("ParamSitPose")){hasSit=true;if(axes.get(i).getKeys()[coord[i]]==0)standing=true;}boolean neutralAdded=!hasSit;for(int i=original.getAxes().size();i<axes.size();i++)if(axes.get(i).getKeys()[coord[i]]!=parameters.get(axisId(axes.get(i))).getDefault())neutralAdded=false;if(protectExisting&&(standing||neutralAdded)&&!equalForm(current.getForm(),value))throw new IllegalArgumentException("Protected SitPose0/default new-axis source slice changed "+key);cells.put(key,new KeyformCell<>(coord,(T)value));}
        return new KeyformGrid<>(axes,new ArrayList<>(cells.values()));
    }
    private static ChannelGrids editChannels(ChannelGrids original,JsonObject spec)throws Exception{
        if(!spec.containsKey("channels"))return original;Map<FormChannel,KeyformGrid<ChannelValue>> map=new LinkedHashMap<>(original.getGridsByChannel());Set<FormChannel> seen=new HashSet<>();
        for(JsonObject row:rows(spec,"channels")){fields(row,"channel","initial_value","append_axes","replace_axes","keyforms");FormChannel channel=FormChannel.valueOf(string(row,"channel"));if(!seen.add(channel))throw new IllegalArgumentException("Duplicate channel");var grid=map.get(channel);if(grid==null){if(!row.containsKey("initial_value")||channel.getValueKind()!=ChannelValueKind.SCALAR)throw new IllegalArgumentException("New scalar channel requires initial_value");grid=new KeyformGrid<>(List.of(),List.of(new KeyformCell<>(new int[]{},new ChannelValue.Scalar(number(row,"initial_value")))));}map.put(channel,editGrid(grid,row,null));}
        return new ChannelGrids(map);
    }
    /** Public master restore: checks its independent Native baseline first. */
    private static void loadMaster(JsonObject plan,Map<String,String> inputs,List<byte[]> pngs,Path out,Path store)throws Exception {
        if(!plan.containsKey("master_source")){if(!rows(plan,"restore_meshes").isEmpty())throw new IllegalArgumentException("Restore needs master source");return;}
        JsonObject spec=object(plan.get("master_source"));fields(spec,"cmo","cmo_sha256","moc","moc_sha256","atlas","atlas_sha256");
        Path cmo=target(string(spec,"cmo")),moc=target(string(spec,"moc")),atlas=target(string(spec,"atlas"));
        for(var pair:List.of(Map.entry(cmo,"cmo_sha256"),Map.entry(moc,"moc_sha256"),Map.entry(atlas,"atlas_sha256"))){if(pair.getKey().startsWith(out)||pair.getKey().startsWith(store)||!sha(pair.getKey()).equals(string(spec,pair.getValue())))throw new IllegalArgumentException("Master input SHA/isolation mismatch");inputs.put(pair.getKey().toString(),sha(pair.getKey()));}
        byte[] image=Files.readAllBytes(atlas);if(!Arrays.equals(image,pngs.get(0)))throw new IllegalArgumentException("Master atlas differs from current page0");
        byte[] cmoBytes=Files.readAllBytes(cmo),mocBytes=Files.readAllBytes(moc);MocDocument reference=Moc3.INSTANCE.read(mocBytes);Map<String,Integer> pages=new LinkedHashMap<>();for(var mesh:reference.getArtMeshes()){if(mesh.getTextureIndex()!=0)throw new IllegalArgumentException("Master single-page source required");pages.put(mesh.getId(),0);}
        var saved=savedAtlasBindings(cmoBytes,List.of(image),pages);masterModel=bind(Cmo3ModelImport.INSTANCE.read(cmoBytes).getPuppet(),saved,reference.getCanvas().getPixelsPerUnit());
        if(!Arrays.equals(nativeMoc(masterModel,reference),mocBytes))throw new IllegalStateException("Master Native baseline not byteexact");
        operationAudit.add(Map.of("kind","master_restore_source","cmo_sha256",sha(cmo),"moc_sha256",sha(moc),"native_baseline_byteexact",true));
    }
    private static Drawable restore(Drawable current,JsonObject spec)throws Exception {
        if(masterModel==null)throw new IllegalStateException("No verified master");String name=id(current);Drawable source=null;for(Drawable d:masterModel.getDrawables())if(id(d).equals(name))source=d;if(source==null)throw new IllegalArgumentException("Master mesh missing "+name);
        Map<String,Object> a=new LinkedHashMap<>((Map<String,Object>)graph(current,0)),b=new LinkedHashMap<>((Map<String,Object>)graph(source,0));
        // Author atlas tile UUIDs are fresh on each legitimate save. Actual
        // page/GUID/PNG bindings and public UVs are checked independently.
        for(String field:List.of("getGeometryGrid","getChannelGrids","getAtlasTileId-lcaijII")){a.remove(field);b.remove(field);}
        if(!a.equals(b)){var differences=new ArrayList<Object>();valueDifferences(a,b,"master_basis/"+name,differences);System.out.println("MASTER_BASIS_DIFFERENCES "+json(differences));throw new IllegalStateException("Master/current mesh basis, UV, topology, parent or intrinsic fields differ "+name);}
        ChannelGrids channels=editChannels(source.getChannelGrids(),spec);
        Drawable repaired=(Drawable)copy(current,Map.of(7,source.getGeometryGrid(),8,channels));
        operationAudit.add(Map.of("kind","restore_master_mesh_geometry_and_all_channels","id",name,"master_geometry",gridSummary(source),"before_geometry",gridSummary(current),"after_geometry",gridSummary(repaired),"master_channels_sha256",hash(json(graph(source.getChannelGrids(),0)).getBytes(java.nio.charset.StandardCharsets.UTF_8))));return repaired;
    }
    /** Clone current expanded floor geometry before restoring old visual nodes. */
    private static List<JsonObject> floorRows(PuppetModel model,JsonObject plan)throws Exception {
        List<JsonObject> result=new ArrayList<>();for(JsonObject spec:rows(plan,"floor_clones")){
            fields(spec,"id","name","source_id","part_id","texture_page");String sourceId=string(spec,"source_id");Drawable source=null;for(Drawable d:model.getDrawables())if(id(d).equals(sourceId))source=d;if(source==null)throw new IllegalArgumentException("Unknown floor source");
            var sourceGraph=(Map<String,Object>)graph(source,0);String parent=(String)sourceGraph.get("getParentDeformerId-lmpY1tE");
            var axes=new ArrayList<Object>();for(KeyformAxis axis:source.getGeometryGrid().getAxes())axes.add(Map.of("parameter",axisId(axis),"keys",graph(axis.getKeys(),0)));
            var cells=new ArrayList<Object>();for(var cell:source.getGeometryGrid().getCells())cells.add(Map.of("coordinate",graph(cell.getCoordinate(),0),"position_deltas",graph(cell.getForm().getPositionDeltas(),0)));
            Map<String,Object> row=new LinkedHashMap<>();row.put("id",string(spec,"id"));row.put("name",string(spec,"name"));row.put("template_id",sourceId);row.put("parent_id",parent);row.put("part_id",string(spec,"part_id"));row.put("texture_page",integer(spec.get("texture_page")));
            row.put("positions",graph(source.getMesh().getPositions(),0));row.put("uvs",graph(source.getMesh().getUvs(),0));row.put("indices",graph(source.getMesh().getIndices(),0));row.put("axes",axes);row.put("keyforms",cells);row.put("draw_order",source.getDrawOrder());row.put("opacity",0);
            row.put("channels",List.of(Map.of("channel","OPACITY","initial_value",0)));
            result.add(object(Json.Default.parseToJsonElement(json(row))));operationAudit.add(Map.of("kind","hidden_floor_geometry_source_clone","id",string(spec,"id"),"source_id",sourceId,"geometry_sha256",hash(json(graph(source.getGeometryGrid(),0)).getBytes(java.nio.charset.StandardCharsets.UTF_8)),"always_hidden",true));
        }return result;
    }
    private static PuppetModel bind(PuppetModel p,Map<String,Integer> pages,float ppu)throws Exception{List<Drawable> ds=new ArrayList<>();for(Drawable d:p.getDrawables()){Integer page=pages.get(id(d));if(page==null)throw new IllegalArgumentException("Missing original/new page binding "+id(d));ds.add((Drawable)copy(d,Map.of(19,page)));}return (PuppetModel)copy(p,Map.of(4,ds,15,ppu));}
    private static PuppetModel apply(PuppetModel p,JsonObject plan,Map<String,Integer> pages,int pageCount)throws Exception{
        List<Parameter> params=new ArrayList<>(p.getParameters());List<ParameterNode> parameterTree=new ArrayList<>(p.getParameterTree());for(Parameter parameter:params)parameters.put(id(parameter),parameter);
        for(JsonObject row:rows(plan,"parameters")){fields(row,"id","name","min","max","default");String name=string(row,"id");float min=number(row,"min"),max=number(row,"max"),def=number(row,"default");if(parameters.containsKey(name)||min>=max||def<min||def>max)throw new IllegalArgumentException("Invalid new parameter "+name);Parameter parameter=(Parameter)valueConstructor(Parameter.class,name,string(row,"name"),min,max,def,ParameterKind.NORMAL,false,null);params.add(parameter);parameterTree.add((ParameterNode)valueConstructor(ParameterNode.Param.class,name));parameters.put(name,parameter);newParameters.add(name);}
        Map<String,JsonObject> warpPlans=new LinkedHashMap<>(),meshPlans=new LinkedHashMap<>();
        for(JsonObject row:rows(plan,"deformer_grids")){fields(row,"id","append_axes","replace_axes","keyforms","channels");if(warpPlans.put(string(row,"id"),row)!=null)throw new IllegalArgumentException("Duplicate deformer edit");}
        for(JsonObject row:rows(plan,"mesh_grids")){fields(row,"id","append_axes","replace_axes","keyforms","channels");if(meshPlans.put(string(row,"id"),row)!=null)throw new IllegalArgumentException("Duplicate mesh edit");}
        List<Deformer> deformers=new ArrayList<>();
        for(Deformer d:p.getDeformers()){String name=id(d);JsonObject spec=warpPlans.remove(name);if(spec==null){deformers.add(d);continue;}Deformer next;if(d instanceof Deformer.Warp w)next=(Deformer)copy(w,Map.of(8,editGrid(w.getGeometryGrid(),spec,null),9,editChannels(w.getChannelGrids(),spec)));else if(d instanceof Deformer.Rotation r)next=(Deformer)copy(r,Map.of(6,editGrid(r.getGeometryGrid(),spec,null),7,editChannels(r.getChannelGrids(),spec)));else throw new IllegalArgumentException("Unsupported deformer type");deformers.add(next);changedDeformers.add(name);operationAudit.add(Map.of("kind","deformer_grid","id",name,"before_geometry",gridSummary(d),"after_geometry",gridSummary(next)));}
        Map<String,JsonObject> restoration=new LinkedHashMap<>();for(JsonObject row:rows(plan,"restore_meshes")){fields(row,"id","channels");if(restoration.put(string(row,"id"),row)!=null)throw new IllegalArgumentException("Duplicate restoration");}
        List<JsonObject> floorRows=floorRows(p,plan);
        List<Drawable> draws=new ArrayList<>();
        for(Drawable d:p.getDrawables()){String name=id(d);JsonObject repair=restoration.remove(name);if(repair!=null){if(meshPlans.containsKey(name))throw new IllegalArgumentException("Restore/grid edit collision");Drawable replacement=restore(d,repair);draws.add(replacement);changedMeshes.add(name);continue;}JsonObject spec=meshPlans.remove(name);if(spec==null){draws.add(d);continue;}Drawable next=(Drawable)copy(d,Map.of(7,editGrid(d.getGeometryGrid(),spec,d.getMesh().getPositions()),8,editChannels(d.getChannelGrids(),spec)));draws.add(next);changedMeshes.add(name);operationAudit.add(Map.of("kind","mesh_grid","id",name,"before_geometry",gridSummary(d),"after_geometry",gridSummary(next)));}
        if(!restoration.isEmpty())throw new IllegalArgumentException("Unknown restore IDs "+restoration.keySet());
        if(!warpPlans.isEmpty()||!meshPlans.isEmpty())throw new IllegalArgumentException("Unknown target IDs "+warpPlans.keySet()+meshPlans.keySet());
        PuppetModel edited=(PuppetModel)copy(p,Map.of(1,params,3,deformers,4,draws,10,parameterTree));
        for(JsonObject row:rows(plan,"insert_meshes"))edited=insert(edited,row,pages,pageCount);
        for(JsonObject row:floorRows)edited=insert(edited,row,pages,pageCount);
        Set<String> referenced=new HashSet<>();for(Object item:edited.getDrawables()){var d=(Drawable)item;for(var a:d.getGeometryGrid().getAxes())referenced.add(axisId(a));for(var g:d.getChannelGrids().getGridsByChannel().values())for(var a:g.getAxes())referenced.add(axisId(a));}for(Deformer d:edited.getDeformers()){for(KeyformAxis a:(List<KeyformAxis>)((KeyformGrid<?>)d.getClass().getMethod("getGeometryGrid").invoke(d)).getAxes())referenced.add(axisId(a));for(var g:d.getChannelGrids().getGridsByChannel().values())for(var a:g.getAxes())referenced.add(axisId(a));}for(String parameter:newParameters)if(!referenced.contains(parameter))throw new IllegalArgumentException("Nonfunctional/unreferenced new parameter "+parameter);
        if(!insertedMeshes.isEmpty())edited=alignInsertedRenderOrder(p,edited);
        return edited;
    }
    // The public CMO writer/readback obtains flat PassThrough render order by
    // traversing the organizational tree backwards. Preserve the existing
    // order exactly and place new nodes in their declared Part segment, rather
    // than appending them outside it and silently accepting a changed order.
    private static void organizationalRender(List<OrgChild> children,Map<String,Part> parts,List<RenderNode> out,Set<String> visited)throws Exception{
        for(int i=children.size()-1;i>=0;i--){OrgChild child=children.get(i);String name=id(child);if(child instanceof OrgChild.Drawable){if(!visited.add("drawable:"+name))throw new IllegalArgumentException("Duplicate organizational drawable "+name);out.add((RenderNode)valueConstructor(RenderDrawable.class,name));}else if(child instanceof OrgChild.Part){Part part=parts.get(name);if(part==null||!visited.add("part:"+name))throw new IllegalArgumentException("Missing/cyclic organizational part "+name);if(!part.getGroupMode().getClass().getSimpleName().equals("PassThrough"))throw new IllegalArgumentException("Inserted meshes currently require proven flat PassThrough CMO organization");organizationalRender(part.getChildren(),parts,out,visited);}else throw new IllegalArgumentException("Unmeasured organizational node "+child.getClass());}
    }
    private static List<RenderNode> flatAuthorRender(PuppetModel model)throws Exception{Map<String,Part> parts=new HashMap<>();for(Part part:model.getParts())parts.put(id(part),part);List<RenderNode> result=new ArrayList<>();organizationalRender(model.getRootChildren(),parts,result,new HashSet<>());return result;}
    private static PuppetModel alignInsertedRenderOrder(PuppetModel before,PuppetModel edited)throws Exception{
        if(!graph(before.getRenderRoot().getChildren(),0).equals(graph(flatAuthorRender(before),0)))throw new IllegalArgumentException("Original render order differs from public flat CMO organization");List<RenderNode> order=flatAuthorRender(edited),oldOnly=new ArrayList<>();for(RenderNode node:order)if(!insertedMeshes.contains(id(node)))oldOnly.add(node);if(!graph(oldOnly,0).equals(graph(before.getRenderRoot().getChildren(),0)))throw new IllegalStateException("Insertion changed original drawable relative render order");RenderGroup root=(RenderGroup)copy(edited.getRenderRoot(),Map.of(3,order));operationAudit.add(Map.of("kind","insert_render_order","old_relative_order_preserved",true,"author_order",graph(order,0)));return (PuppetModel)copy(edited,Map.of(8,root));
    }
    private static Object gridSummary(Object value)throws Exception{KeyformGrid<?> grid=(KeyformGrid<?>)value.getClass().getMethod("getGeometryGrid").invoke(value);return Map.of("axes",graph(grid.getAxes(),0),"forms",grid.getCells().size());}
    private static PuppetModel insert(PuppetModel p,JsonObject row,Map<String,Integer> pages,int pageCount)throws Exception{
        fields(row,"id","name","template_id","parent_id","part_id","texture_page","positions","uvs","indices","axes","keyforms","channels","draw_order","opacity");String name=string(row,"id"),template=string(row,"template_id"),parent=string(row,"parent_id"),part=string(row,"part_id");Drawable original=null;for(Drawable d:p.getDrawables()){if(id(d).equals(name))throw new IllegalArgumentException("New mesh ID already exists");if(id(d).equals(template))original=d;}if(original==null)throw new IllegalArgumentException("Unknown insertion template");boolean parentFound=false;for(Deformer d:p.getDeformers())if(id(d).equals(parent))parentFound=true;if(!parentFound)throw new IllegalArgumentException("Unknown parent deformer");
        int page=integer(row.get("texture_page"));integer(row.get("draw_order"));if(page<0||page>=pageCount)throw new IllegalArgumentException("Invalid new page");float[] positions=floats(row.get("positions")),uv=floats(row.get("uvs"));int[] indices=ints(row.get("indices"));if(positions.length<6||positions.length%2!=0||uv.length!=positions.length||indices.length%3!=0)throw new IllegalArgumentException("Invalid inserted mesh layout");for(float v:uv)if(v<0||v>1)throw new IllegalArgumentException("UV outside unit page");for(int i:indices)if(i<0||i>=positions.length/2)throw new IllegalArgumentException("Invalid triangle index");
        List<KeyformAxis> axes=new ArrayList<>();Set<String> unique=new HashSet<>();for(JsonObject a:rows(row,"axes")){fields(a,"parameter","keys");KeyformAxis ax=axis(a);if(!unique.add(axisId(ax)))throw new IllegalArgumentException("Duplicate inserted axis");axes.add(ax);}List<KeyformCell<MeshDeltaForm>> cells=new ArrayList<>();for(int[] c:coordinates(axes))cells.add(new KeyformCell<>(c,new MeshDeltaForm(new float[positions.length])));var seeded=new KeyformGrid<>(axes,cells);var geometry=editGrid(seeded,row,positions,false);
        Map<Integer,Object> changes=new HashMap<>();changes.put(1,name);changes.put(2,string(row,"name"));changes.put(3,parent);changes.put(5,List.of());changes.put(6,new DrawableMesh(positions,uv,indices));changes.put(7,geometry);changes.put(8,editChannels(new ChannelGrids(Map.of()),row));changes.put(9,number(row,"draw_order"));changes.put(10,number(row,"opacity"));changes.put(11,new ColorRgb(1,1,1));changes.put(12,new ColorRgb(0,0,0));changes.put(13,false);changes.put(15,false);changes.put(16,true);changes.put(17,true);changes.put(18,null);changes.put(19,page);changes.put(20,null);changes.put(21,List.of());changes.put(22,"");Drawable mesh=(Drawable)copy(original,changes);
        List<Drawable> ds=new ArrayList<>(p.getDrawables());ds.add(mesh);List<Part> parts=new ArrayList<>();boolean found=false;for(Part old:p.getParts()){if(!id(old).equals(part)){parts.add(old);continue;}found=true;var children=new ArrayList<>(old.getChildren());children.add((OrgChild)valueConstructor(OrgChild.Drawable.class,name));parts.add((Part)copy(old,Map.of(3,children)));}if(!found)throw new IllegalArgumentException("Unknown insertion part");
        RenderGroup render=p.getRenderRoot();int groupCount=renderPartCount(render,part);if(groupCount>1)throw new IllegalArgumentException("Ambiguous insertion render group");if(groupCount==1)render=addRender(render,part,name);else{Part targetPart=parts.stream().filter(x->{try{return id(x).equals(part);}catch(Exception e){throw new IllegalStateException(e);}}).findFirst().orElseThrow();if(!targetPart.getGroupMode().getClass().getSimpleName().equals("PassThrough"))throw new IllegalArgumentException("Missing grouped insertion render part");var children=new ArrayList<>(render.getChildren());children.add((RenderNode)valueConstructor(RenderDrawable.class,name));render=(RenderGroup)copy(render,Map.of(3,children));}PuppetModel result=(PuppetModel)copy(p,Map.of(2,parts,4,ds,8,render));pages.put(name,page);insertedMeshes.add(name);operationAudit.add(Map.of("kind","insert_mesh","id",name,"parent",parent,"part",part,"texture_page",page,"vertices",positions.length/2,"triangles",indices.length/3));return result;
    }
    private static int renderPartCount(RenderGroup group,String part)throws Exception{if(group==null)throw new IllegalArgumentException("Missing render root");int count=Objects.equals(group.getClass().getMethod("getPartId-wrCY1ZY").invoke(group),part)?1:0;for(RenderNode n:group.getChildren())if(n instanceof RenderGroup g)count+=renderPartCount(g,part);return count;}
    private static RenderGroup addRender(RenderGroup group,String part,String drawable)throws Exception{if(group==null)throw new IllegalArgumentException("Missing render root");String name=(String)group.getClass().getMethod("getPartId-wrCY1ZY").invoke(group);List<RenderNode> children=new ArrayList<>();boolean match=Objects.equals(name,part);for(RenderNode node:group.getChildren())children.add(node instanceof RenderGroup g?addRender(g,part,drawable):node);if(match)children.add((RenderNode)valueConstructor(RenderDrawable.class,drawable));return (RenderGroup)copy(group,Map.of(3,children));}
    private static Object insertionSummary(PuppetModel model)throws Exception {var meshes=new ArrayList<Object>();for(Drawable d:model.getDrawables())if(insertedMeshes.contains(id(d))){var row=new LinkedHashMap<String,Object>();row.put("id",id(d));row.put("geometry",graph(d.getGeometryGrid(),0));row.put("channels",graph(d.getChannelGrids(),0));meshes.add(row);};return Map.of("parameters",graph(model.getParameters(),0),"parameter_tree",graph(model.getParameterTree(),0),"meshes",meshes);}
    private static void valueDifferences(Object a,Object b,String path,List<Object> out){if(Objects.equals(a,b))return;if(a instanceof Map<?,?> ma&&b instanceof Map<?,?> mb&&ma.keySet().equals(mb.keySet())){for(Object k:ma.keySet())valueDifferences(ma.get(k),mb.get(k),path+"/"+k,out);return;}if(a instanceof List<?> la&&b instanceof List<?> lb&&la.size()==lb.size()){for(int i=0;i<la.size();i++)valueDifferences(la.get(i),lb.get(i),path+"/"+i,out);return;}var row=new LinkedHashMap<String,Object>();row.put("path",path);row.put("before",a);row.put("after",b);out.add(row);}
    // Public graph/canonical/path helpers are inserted below from the audited recovery helper.
    private static Object graph(Object v,int depth) throws Exception {
        if(v==null||v instanceof String||v instanceof Number||v instanceof Boolean) return v;
        if(depth>40) throw new IllegalStateException("Graph depth limit");
        if(v instanceof Enum<?> e) return e.name();
        if(v.getClass().isArray()) {
            List<Object> r=new ArrayList<>(); for(int i=0;i<Array.getLength(v);i++) r.add(graph(Array.get(v,i),depth+1)); return r;
        }
        if(v instanceof Iterable<?> a) {
            List<Object> r=new ArrayList<>(); for(Object e:a) r.add(graph(e,depth+1)); return r;
        }
        if(v instanceof Map<?,?> a) {
            Map<String,Object> r=new java.util.TreeMap<>(); for(var e:a.entrySet()) r.put(e.getKey().toString(),graph(e.getValue(),depth+1)); return r;
        }
        if(!v.getClass().getName().startsWith("org.umamo.runtime.model.")&&!v.getClass().getName().startsWith("org.umamo.format.moc3."))
            throw new IllegalStateException("Unmeasured runtime value: "+v.getClass().getName());
        Map<String,Object> r=new LinkedHashMap<>(); r.put("type",v.getClass().getName());
        List<String> excluded=List.of("getClass","getPartById","getCellsByLinearIndex");
        var methods=java.util.Arrays.stream(v.getClass().getMethods()).filter(m -> Modifier.isPublic(m.getModifiers())
            && !Modifier.isStatic(m.getModifiers())&&m.getParameterCount()==0
            &&(m.getName().startsWith("get")||m.getName().startsWith("is"))&&!excluded.contains(m.getName()))
            .sorted(Comparator.comparing(Method::getName)).toList();
        for(Method m:methods) r.put(m.getName(),graph(m.invoke(v),depth+1));
        if(methods.isEmpty()&&!List.of("org.umamo.runtime.model.ParameterLabelColor$None",
            "org.umamo.runtime.model.PartGroupMode$PassThrough").contains(v.getClass().getName()))
            throw new IllegalStateException("Unmeasured singleton: "+v.getClass().getName());
        return r;
    }
    private static List<String> ids(List<?> rows) throws Exception {
        List<String> out=new ArrayList<>();for(Object row:rows)out.add((String)row.getClass().getMethod("getId").invoke(row));return out;
    }
    private static Map<String,Object> binding(MocDocument doc,int index) throws Exception {
        Map<String,Object> out=(Map<String,Object>)graph(doc.keyformBinding(index),0);out.remove("getIndex");
        List<String> params=ids(doc.getParameters());
        for(Object row:(List<?>)out.get("getAxes")) {
            Map<String,Object> axis=(Map<String,Object>)row;
            axis.put("parameter_id",params.get(((Number)axis.remove("getParameterIndex")).intValue()));
        }
        return out;
    }
    private static Map<String,Object> named(MocDocument doc,List<?> rows) throws Exception {
        List<String> parts=ids(doc.getParts()),deformers=ids(doc.getDeformers()),meshes=ids(doc.getArtMeshes());
        Map<String,Object> out=new java.util.TreeMap<>();
        for(Object value:rows) {
            Map<String,Object> row=(Map<String,Object>)graph(value,0);
            row.put("keyform_binding",binding(doc,((Number)row.remove("getKeyformBindingIndex")).intValue()));
            if(row.containsKey("getParentPartIndex")) {
                int i=((Number)row.remove("getParentPartIndex")).intValue();row.put("parent_part_id",i<0?null:parts.get(i));
            }
            if(row.containsKey("getParentDeformerIndex")) {
                int i=((Number)row.remove("getParentDeformerIndex")).intValue();row.put("parent_deformer_id",i<0?null:deformers.get(i));
            }
            if(row.containsKey("getMaskDrawableIndices")) {
                List<String> mask=new ArrayList<>();for(Object i:(List<?>)row.remove("getMaskDrawableIndices"))mask.add(meshes.get(((Number)i).intValue()));
                row.put("mask_drawable_ids",mask);
            }
            out.put(row.get("getId").toString(),row);
        }
        return out;
    }
    @SuppressWarnings("unchecked")
    private static Map<String,Object> parameterMap(MocDocument doc)throws Exception {var out=new TreeMap<String,Object>();for(var p:doc.getParameters())out.put(p.getId(),graph(p,0));return out;}
    private static Object semantics(MocDocument doc) throws Exception {
        if(!doc.getGlues().isEmpty()||!doc.getBlendShapes().isEmpty()||!doc.getOffscreens().isEmpty())
            throw new IllegalStateException("Unmeasured extra runtime feature");
        Map<String,Object> out=new java.util.TreeMap<>();
        out.put("version",doc.getVersion().name());out.put("parameter_union",doc.getKeyPositionsHasParameterUnion());
        out.put("canvas",graph(doc.getCanvas(),0));out.put("parameters",parameterMap(doc));
        out.put("parts",named(doc,doc.getParts()));out.put("deformers",named(doc,doc.getDeformers()));out.put("meshes",named(doc,doc.getArtMeshes()));
        List<Object> bindings=new ArrayList<>();for(var b:doc.getBindings())bindings.add(binding(doc,b.getIndex()));
        bindings.sort(Comparator.comparing(PSD2LiveStandingRestore::json));out.put("bindings",bindings);
        Object groups=graph(doc.getRenderOrderGroups(),0);List<String> meshIds=ids(doc.getArtMeshes());
        for(Object value:(List<?>)groups)for(Object child:(List<?>)((Map<?,?>)value).get("getChildren")) {
            Map<String,Object> row=(Map<String,Object>)child;
            if(((Number)row.get("getKind")).intValue()!=0)throw new IllegalStateException("Unmeasured render child kind");
            row.put("drawable_id",meshIds.get(((Number)row.remove("getIndex")).intValue()));
        }
        out.put("render_groups",groups);return out;
    }
    private static Path target(String value) throws Exception {
        Path p=Path.of(value);if(!p.isAbsolute())throw new IllegalArgumentException("Absolute paths required");
        p=p.normalize();Path existing=p;List<Path> suffix=new ArrayList<>();
        while(!Files.exists(existing,java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            suffix.add(existing.getFileName());existing=existing.getParent();
            if(existing==null)throw new IllegalArgumentException("No real ancestor");
        }
        Path resolved=existing.toRealPath();for(int i=suffix.size()-1;i>=0;i--)resolved=resolved.resolve(suffix.get(i));
        return resolved.normalize();
    }
    private static boolean overlap(Path a,Path b){return a.startsWith(b)||b.startsWith(a);}
    private static void fresh(Path path)throws Exception{if(Files.exists(path))throw new IllegalArgumentException("Fresh path required "+path);Files.createDirectories(path);}
    private static byte[] nativeMoc(PuppetModel p,MocDocument reference)throws Exception{var result=Moc3Export.INSTANCE.write(p,reference.getVersion(),null,new Moc3ExportOptions(true,true,true,false,false,true,reference.getCanvas().getPixelsPerUnit()));if(!result.getSecond().isEmpty())throw new IllegalStateException("Native export notices "+result.getSecond());return result.getFirst();}
    private static List<Object> embeddedPageEvidence(byte[] saved,List<byte[]> pages)throws Exception{
        var author=Cmo3.INSTANCE.read(saved);Map<String,String> resources=new LinkedHashMap<>();for(var image:author.imageResources())resources.put(hash(author.extractLayerPng(image)),image.getImageFileBuf().getArchivePath());List<Object> evidence=new ArrayList<>();for(int i=0;i<pages.size();i++){String signature=hash(pages.get(i)),resource=resources.get(signature);if(resource==null)throw new IllegalStateException("Saved CMO does not embed exact input atlas PNG page "+i);evidence.add(Map.of("page",i,"png_sha256",signature,"embedded_resource",resource,"input_png_bytes_exact",true));}return evidence;
    }
    /** Validate actual saved author-page references before restoring importer's
     * omitted Native page indices. Duplicate byte-identical PNG pages are legal,
     * but must have distinct atlas GUIDs and the declared per-mesh binding.
     * This reads the saved CMO; it never repairs readback UVs or texture links.
     */
    private static Map<String,Integer> savedAtlasBindings(byte[] saved,List<byte[]> pngs,Map<String,Integer> declared)throws Exception{
        var author=Cmo3.INSTANCE.read(saved);var root=(CModelSource)author.getRoot();var manager=(CTextureManager)root.getTextureManager();
        var atlases=(List<?>)manager.get_textureAtlases();if(atlases.size()!=pngs.size())throw new IllegalStateException("Saved CMO atlas page count changed: expected "+pngs.size()+", got "+atlases.size()+"; implicit atlas undedup/repack is forbidden");
        Map<String,Integer> pageByGuid=new LinkedHashMap<>();
        for(int page=0;page<atlases.size();page++){var atlas=(CTextureAtlas)atlases.get(page);String guid=((Guid)atlas.getGuid()).getUuid();if(pageByGuid.put(guid,page)!=null)throw new IllegalStateException("Saved CMO duplicate atlas GUID");var resource=(CImageResource)atlas.getCachedAtlasImage();if(!Arrays.equals(author.extractLayerPng(resource),pngs.get(page)))throw new IllegalStateException("Saved CMO atlas page PNG changed "+page);}
        Map<String,Integer> actual=new LinkedHashMap<>();var sources=(CDrawableSourceSet)root.getDrawableSourceSet();
        for(Object value:(List<?>)sources.get_sources()){if(!(value instanceof CArtMeshSource mesh))throw new IllegalStateException("Unmeasured saved drawable type");String name=((Id)mesh.getId()).getIdstr();Integer expected=declared.get(name);if(expected==null)throw new IllegalStateException("Saved CMO undeclared drawable "+name);Integer page=null;for(Object extension:(List<?>)mesh.get_extensions())if(extension instanceof CTextureInputExtension texture){if(!(texture.getCurrentTextureInputData() instanceof CTextureInput_TextureAtlasRegion region))throw new IllegalStateException("Saved CMO drawable is not atlas-region mode "+name);if(page!=null)throw new IllegalStateException("Multiple saved CMO texture bindings "+name);page=pageByGuid.get(((Guid)region.getTextureAtlasGuid()).getUuid());}
            if(page==null||!page.equals(expected))throw new IllegalStateException("Saved CMO drawable atlas binding changed "+name+": expected "+expected+", got "+page);if(!(mesh.getTexture() instanceof GTexture2D texture)||!(texture.getSrcImageResource() instanceof CImageResource resource)||!Arrays.equals(author.extractLayerPng(resource),pngs.get(page)))throw new IllegalStateException("Saved CMO drawable actual texture PNG changed "+name);if(actual.put(name,page)!=null)throw new IllegalStateException("Saved CMO duplicate drawable "+name);}
        if(!actual.keySet().equals(declared.keySet()))throw new IllegalStateException("Saved CMO drawable set changed during atlas export");return actual;
    }
    @SuppressWarnings("unchecked")
    private static List<Object> limitChanges(Object before,Object after)throws Exception{
        Map<String,Object> a=(Map<String,Object>)before,b=(Map<String,Object>)after;var changes=new ArrayList<Object>();
        for(String group:List.of("meshes","deformers","parts","parameters")){Map<String,Object> left=(Map<String,Object>)a.get(group),right=(Map<String,Object>)b.get(group);for(var entry:left.entrySet()){String name=entry.getKey();if(!right.containsKey(name))throw new IllegalStateException("Old runtime node removed "+name);if(!Objects.equals(entry.getValue(),right.get(name))){boolean allowed=group.equals("meshes")?changedMeshes.contains(name):group.equals("deformers")?changedDeformers.contains(name):group.equals("parts")&&!insertedMeshes.isEmpty();if(!allowed)throw new IllegalStateException("Untargeted runtime node changed "+group+"/"+name);changes.add(Map.of("kind",group,"id",name,"before_sha256",hash(json(entry.getValue()).getBytes(java.nio.charset.StandardCharsets.UTF_8)),"after_sha256",hash(json(right.get(name)).getBytes(java.nio.charset.StandardCharsets.UTF_8))));}}for(String name:right.keySet())if(!left.containsKey(name)){if(!(group.equals("meshes")&&insertedMeshes.contains(name)||group.equals("parameters")&&newParameters.contains(name)))throw new IllegalStateException("Undeclared runtime node inserted "+name);changes.add(Map.of("kind",group,"id",name,"inserted",true));}}
        for(String key:List.of("canvas","version","parameter_union"))if(!Objects.equals(a.get(key),b.get(key)))throw new IllegalStateException("Protected global runtime field changed "+key);
        if(insertedMeshes.isEmpty()&&!Objects.equals(a.get("render_groups"),b.get("render_groups")))throw new IllegalStateException("Protected render group changed");return changes;
    }
    private static void addPngInput(JsonObject row,List<byte[]> pngs,Map<String,String> inputs,Path out,Path store)throws Exception {
        fields(row,"path","sha256");Path path=target(string(row,"path"));
        if(path.startsWith(out)||path.startsWith(store))throw new IllegalArgumentException("PNG input/output overlap");
        String signature=sha(path);if(!signature.equals(string(row,"sha256")))throw new IllegalArgumentException("PNG SHA mismatch "+path);
        pngs.add(Files.readAllBytes(path));inputs.put(path.toString(),signature);
    }
    public static void main(String[] args)throws Exception {
        Set<String> allowed=Set.of("--install","--cmo","--reference-moc","--atlas","--edit-json","--output","--store");
        Set<String> required=Set.of("--install","--cmo","--reference-moc","--edit-json","--output","--store");
        Map<String,String> options=new LinkedHashMap<>();
        for(int i=0;i<args.length;i+=2)if(i+1>=args.length||!allowed.contains(args[i])||options.put(args[i],args[i+1])!=null)throw new IllegalArgumentException("Expected unique option/value pairs "+allowed);
        if(!options.keySet().containsAll(required))throw new IllegalArgumentException("Required options "+required);
        Path install=target(options.get("--install")),cmo=target(options.get("--cmo")),moc=target(options.get("--reference-moc")),planPath=target(options.get("--edit-json")),out=target(options.get("--output")),store=target(options.get("--store"));
        if(overlap(out,store)||overlap(out,install)||overlap(store,install))throw new IllegalArgumentException("Isolation overlap");
        for(Path input:List.of(cmo,moc,planPath))if(input.startsWith(out)||input.startsWith(store))throw new IllegalArgumentException("Source/output overlap");
        String local=System.getenv("LOCALAPPDATA");Path defaultStore=target((local==null?Path.of(System.getProperty("user.home"),".psd2live","agent-workspaces"):Path.of(local,"PSD2Live","agent-workspaces")).toAbsolutePath().toString());
        if(overlap(out,defaultStore)||overlap(store,defaultStore))throw new IllegalArgumentException("Default author store forbidden");
        String property=System.getProperty("psd2live.agent.store");if(property==null||!target(property).equals(store))throw new IllegalArgumentException("Isolated store JVM property required");
        Path jar=install.resolve("app").resolve(JAR_NAME);
        if(!sha(jar).equals(JAR_SHA)||!Path.of(Moc3.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath().equals(jar.toRealPath()))throw new IllegalStateException("Loaded JAR/version mismatch");
        JsonObject plan=object(Json.Default.parseToJsonElement(Files.readString(planPath)));
        fields(plan,"schema_version","source","parameters","deformer_grids","mesh_grids","append_pages","insert_meshes","description","provenance","kind","coordinate_contract","authoredSitPose","limits","master_source","restore_meshes","floor_clones");
        int version=integer(plan.get("schema_version"));if(version!=1&&version!=2)throw new IllegalArgumentException("Unsupported plan version");
        JsonObject source=object(plan.get("source"));
        if(version==1)fields(source,"cmo_sha256","moc_sha256","atlas_sha256");else fields(source,"cmo_sha256","moc_sha256","atlas_pages");
        if(!sha(cmo).equals(string(source,"cmo_sha256"))||!sha(moc).equals(string(source,"moc_sha256")))throw new IllegalArgumentException("Source CMO/MOC SHA mismatch");
        Map<String,String> inputs=new LinkedHashMap<>();for(Path p:List.of(cmo,moc,planPath,jar))inputs.put(p.toString(),sha(p));
        List<byte[]> pngs=new ArrayList<>();
        if(version==1){
            if(!options.containsKey("--atlas"))throw new IllegalArgumentException("schema1 requires --atlas");
            Path atlas=target(options.get("--atlas"));if(atlas.startsWith(out)||atlas.startsWith(store))throw new IllegalArgumentException("Atlas input/output overlap");
            if(!sha(atlas).equals(string(source,"atlas_sha256")))throw new IllegalArgumentException("Source atlas SHA mismatch");
            pngs.add(Files.readAllBytes(atlas));inputs.put(atlas.toString(),sha(atlas));
        }else{
            List<JsonObject> sourcePages=rows(source,"atlas_pages");if(sourcePages.isEmpty())throw new IllegalArgumentException("schema2 requires ordered source.atlas_pages");
            for(JsonObject row:sourcePages)addPngInput(row,pngs,inputs,out,store);
            if(options.containsKey("--atlas")&&!target(options.get("--atlas")).equals(target(string(sourcePages.get(0),"path"))))throw new IllegalArgumentException("Optional --atlas must be schema2 source page0 path");
        }
        int sourcePageCount=pngs.size();byte[] sourceCmoBytes=Files.readAllBytes(cmo),referenceBytes=Files.readAllBytes(moc);
        MocDocument reference=Moc3.INSTANCE.read(referenceBytes);Map<String,Integer> declaredSourcePages=new LinkedHashMap<>();
        for(var mesh:reference.getArtMeshes()){int page=mesh.getTextureIndex();if(page<0||page>=sourcePageCount)throw new IllegalArgumentException("Reference MOC texture index out of source page domain "+mesh.getId());declaredSourcePages.put(mesh.getId(),page);}
        // Verify real source CMO atlas/GUID/PNG links; derive omitted importer page indices from those links.
        Map<String,Integer> pagesById=savedAtlasBindings(sourceCmoBytes,pngs,declaredSourcePages);
        PuppetModel before=bind(Cmo3ModelImport.INSTANCE.read(sourceCmoBytes).getPuppet(),pagesById,reference.getCanvas().getPixelsPerUnit());
        byte[] baseline=nativeMoc(before,reference);if(!Arrays.equals(baseline,referenceBytes))throw new IllegalStateException("Source CMO Native baseline is not byte-exact reference MOC");
        for(JsonObject row:rows(plan,"append_pages"))addPngInput(row,pngs,inputs,out,store);
        List<Cmo3Conversion.AtlasPage> pages=new ArrayList<>();
        for(byte[] png:pngs){var image=javax.imageio.ImageIO.read(new java.io.ByteArrayInputStream(png));if(image==null||image.getWidth()<1||image.getHeight()<1||image.getWidth()>8192||image.getHeight()>8192)throw new IllegalArgumentException("Invalid/oversize PNG page");pages.add(new Cmo3Conversion.AtlasPage(png,image.getWidth(),image.getHeight()));}
        fresh(out);fresh(store);
        loadMaster(plan,inputs,pngs,out,store);
        PuppetModel edited=apply(before,plan,pagesById,pages.size());Files.writeString(out.resolve("diagnostic-before-CMO.json"),json(insertionSummary(edited))+"\n");
        System.out.println("EXPORT_DIRECT_EDITED_MOC");byte[] direct=nativeMoc(edited,reference);
        var author=Cmo3Conversion.INSTANCE.freshCmo3(edited,pages,pagesById,"Whale girl public multipage parent-local edit",0L,30,tile->null,null);
        byte[] saved=Cmo3.INSTANCE.write(author.getModel());List<Object> embeddedPages=embeddedPageEvidence(saved,pngs);
        Map<String,Integer> savedPages=savedAtlasBindings(saved,pngs,pagesById);Files.write(out.resolve("diagnostic-author.cmo3"),saved);
        System.out.println("CMO_NOTICES "+author.getReport());
        PuppetModel readback=bind(Cmo3ModelImport.INSTANCE.read(saved).getPuppet(),savedPages,reference.getCanvas().getPixelsPerUnit());
        Files.writeString(out.resolve("diagnostic-after-CMO.json"),json(insertionSummary(readback))+"\n");System.out.println("EXPORT_READBACK_MOC");
        byte[] exported=nativeMoc(readback,reference);Object directValues=semantics(Moc3.INSTANCE.read(direct)),readbackValues=semantics(Moc3.INSTANCE.read(exported));
        if(!directValues.equals(readbackValues)){var differences=new ArrayList<Object>();valueDifferences(directValues,readbackValues,"",differences);Files.writeString(out.resolve("diagnostic-readback-differences.json"),json(differences)+"\n");throw new IllegalStateException("Saved CMO public readback changed direct edited Native keyforms: "+differences.size()+" differences; see diagnostic-readback-differences.json");}
        boolean noOp=operationAudit.isEmpty();if(noOp&&!Arrays.equals(exported,referenceBytes))throw new IllegalStateException("No-op saved CMO Native re-export is not byte-exact reference MOC");
        List<Object> changed=limitChanges(semantics(reference),readbackValues);
        Files.write(out.resolve("edited.cmo3"),saved);Files.write(out.resolve("edited.moc3"),exported);List<String> textureNames=new ArrayList<>();
        for(int i=0;i<pngs.size();i++){String name=String.format("texture_%02d.png",i);Files.write(out.resolve(name),pngs.get(i));textureNames.add(name);}
        Files.writeString(out.resolve("edited.model3.json"),json(Map.of("Version",3,"FileReferences",Map.of("Moc","edited.moc3","Textures",textureNames)))+"\n");
        for(var e:inputs.entrySet())if(!sha(Path.of(e.getKey())).equals(e.getValue()))throw new IllegalStateException("Inputs changed during author edit "+e.getKey());
        Map<String,Object> report=new LinkedHashMap<>();report.put("schema_version",2);report.put("edit_plan_schema_version",version);report.put("kind","public_multipage_parent_local_CMO_edit_closed_loop");
        report.put("input_sha256",inputs);report.put("inputs_rechecked_after",true);report.put("source_atlas_pages",sourcePageCount);report.put("appended_atlas_pages",pngs.size()-sourcePageCount);report.put("source_CMO_atlas_binding_matches_reference_MOC",true);report.put("source_CMO_page_index_by_drawable_id",declaredSourcePages);
        report.put("baseline_MOC_byte_exact",true);report.put("no_op",noOp);report.put("no_op_saved_CMO_reexport_MOC_byte_exact",noOp?true:null);
        report.put("direct_vs_saved_CMO_readback_all_runtime_values_exact",true);report.put("untargeted_runtime_nodes_equal",true);report.put("targeted_runtime_changes",changed);report.put("operations",operationAudit);
        report.put("new_parameters",new ArrayList<>(newParameters));report.put("inserted_meshes",new ArrayList<>(insertedMeshes));report.put("atlas_pages",pngs.size());report.put("cmo_exact_embedded_png_pages",embeddedPages);report.put("saved_CMO_atlas_binding_matches_declared",true);report.put("saved_CMO_page_index_by_drawable_id",savedPages);report.put("readback_page_indices_restored_from_saved_CMO",true);
        report.put("cmo_notices",author.getReport().toString());report.put("runtime_export_notices","[]");report.put("domain","caller-resolved existing parent-local mesh/grid coordinates; ordered original atlas pages retained; no PSD rebinding/global rest transform");report.put("approval",Map.of("engineering","limited-change public CMO author roundtrip","visual","pending native captures","adoption","not_automatic"));
        Map<String,String> outputs=new LinkedHashMap<>();for(String name:List.of("edited.cmo3","edited.moc3","edited.model3.json"))outputs.put(name,sha(out.resolve(name)));for(String name:textureNames)outputs.put(name,sha(out.resolve(name)));report.put("output_sha256",outputs);
        Files.writeString(out.resolve("edit-report.json"),json(report)+"\n");System.out.println("PUBLIC_PARENT_EDIT_VALIDATED "+out.resolve("edit-report.json"));
    }
}
