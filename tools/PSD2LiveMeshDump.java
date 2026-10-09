import io.github.psd2live.core.Cmo3ModelImport;
import org.umamo.runtime.model.*;
import java.nio.file.*;
import java.util.*;

/**
 * Dump parent-local geometry of chosen art meshes from an author CMO as JSON:
 * positions, uvs, keyform axes and per-cell position deltas. Read-only.
 * Usage: PSD2LiveMeshDump --install <dir> --cmo <file> --ids A,B --output <json>
 */
public final class PSD2LiveMeshDump {
    private static String id(Object value)throws Exception{return (String)Arrays.stream(value.getClass().getMethods()).filter(m->m.getParameterCount()==0&&m.getName().startsWith("getId-")).findFirst().orElseThrow().invoke(value);}
    private static Object call(Object value,String prefix)throws Exception{return Arrays.stream(value.getClass().getMethods()).filter(m->m.getParameterCount()==0&&m.getName().startsWith(prefix)).findFirst().orElseThrow().invoke(value);}
    private static List<Object> array(Object a){List<Object> out=new ArrayList<>();for(int i=0;i<java.lang.reflect.Array.getLength(a);i++){Object v=java.lang.reflect.Array.get(a,i);out.add(v instanceof Float f?Math.round(f*1e6f)/1e6f:v);}return out;}
    private static String quote(String s){var out=new StringBuilder("\"");for(char c:s.toCharArray()){if(c=='"'||c=='\\')out.append('\\');else if(c<32)out.append(String.format("\\u%04x",(int)c));else out.append(c);}return out.append('"').toString();}
    private static String json(Object v){if(v==null)return "null";if(v instanceof Number||v instanceof Boolean)return v.toString();if(v instanceof Map<?,?> m){var out=new ArrayList<String>();for(var e:m.entrySet())out.add(quote(e.getKey().toString())+":"+json(e.getValue()));return "{"+String.join(",",out)+"}";}if(v instanceof List<?> l){var out=new ArrayList<String>();for(Object e:l)out.add(json(e));return "["+String.join(",",out)+"]";}return quote(v.toString());}
    public static void main(String[] args)throws Exception{
        Map<String,String> o=new LinkedHashMap<>();
        for(int i=0;i<args.length;i+=2)o.put(args[i],args[i+1]);
        Path cmo=Path.of(o.get("--cmo"));
        List<String> want=Arrays.asList(o.get("--ids").split(","));
        PuppetModel p=Cmo3ModelImport.INSTANCE.read(Files.readAllBytes(cmo)).getPuppet();
        List<Object> out=new ArrayList<>();
        for(Drawable d:p.getDrawables()){
            String name=id(d);
            if(!want.contains(name))continue;
            Map<String,Object> row=new LinkedHashMap<>();
            row.put("id",name);
            row.put("parent",(String)call(d,"getParentDeformerId"));
            row.put("draw_order",((Number)call(d,"getDrawOrder")).intValue());
            row.put("opacity",((Number)call(d,"getOpacity")).floatValue());
            var mesh=d.getMesh();
            row.put("uvs",array(call(mesh,"getUvs")));
            row.put("indices",array(call(mesh,"getIndices")));
            float[] pos=d.getMesh().getPositions();
            row.put("positions",array(pos));
            row.put("vertex_count",pos.length/2);
            var grid=d.getGeometryGrid();
            List<Object> axes=new ArrayList<>();
            for(var a:grid.getAxes())axes.add(Map.of("parameter",(String)call(a,"getParameterId"),"keys",array(a.getKeys())));
            row.put("axes",axes);
            List<Object> cells=new ArrayList<>();
            for(var cell:grid.getCells()){
                Map<String,Object> c=new LinkedHashMap<>();
                c.put("coordinate",array(cell.getCoordinate()));
                Object form=cell.getForm();
                if(form instanceof MeshDeltaForm m)c.put("position_deltas",array(m.getPositionDeltas()));
                else c.put("form_type",form.getClass().getName());
                cells.add(c);
            }
            row.put("cells",cells);
            out.add(row);
        }
        if(out.size()!=want.size())throw new IllegalStateException("Missing requested ids: requested "+want+" found "+out.size());
        Files.writeString(Path.of(o.get("--output")),json(out));
        System.out.println("MESH_DUMP_OK "+o.get("--output"));
    }
}
