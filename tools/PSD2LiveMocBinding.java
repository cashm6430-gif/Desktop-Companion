import org.umamo.format.moc3.Moc3;
import org.umamo.format.moc3.MocDocument;
import java.lang.reflect.Array;
import java.nio.file.*;
import java.util.*;

/**
 * Read-only MOC binding diagnostic: for each requested art mesh id, dump its
 * keyform binding axes (resolved parameter ids, key values) and a delta
 * summary (min/max across cells) to locate lost keyform axes after export.
 * Usage: PSD2LiveMocBinding --moc <file> --ids A,B --output <json>
 */
public final class PSD2LiveMocBinding {
    private static String quote(String s){var out=new StringBuilder("\"");for(char c:s.toCharArray()){if(c=='"'||c=='\\')out.append('\\');else if(c<32)out.append(String.format("\\u%04x",(int)c));else out.append(c);}return out.append('"').toString();}
    private static String json(Object v){if(v==null)return "null";if(v instanceof Number||v instanceof Boolean)return v.toString();if(v instanceof Map<?,?> m){var out=new ArrayList<String>();for(var e:m.entrySet())out.add(quote(e.getKey().toString())+":"+json(e.getValue()));return "{"+String.join(",",out)+"}";}if(v instanceof List<?> l){var out=new ArrayList<String>();for(Object e:l)out.add(json(e));return "["+String.join(",",out)+"]";}return quote(v.toString());}
    private static Object graph(Object v,int depth) throws Exception {
        if(v==null||v instanceof String||v instanceof Number||v instanceof Boolean) return v;
        if(depth>40) throw new IllegalStateException("Graph depth limit");
        if(v instanceof Enum<?> e) return e.name();
        if(v.getClass().isArray()){List<Object> r=new ArrayList<>();for(int i=0;i<Array.getLength(v);i++)r.add(graph(Array.get(v,i),depth+1));return r;}
        if(v instanceof Iterable<?> a){List<Object> r=new ArrayList<>();for(Object e:a)r.add(graph(e,depth+1));return r;}
        if(!v.getClass().getName().startsWith("org.umamo.")) throw new IllegalStateException("Unmeasured runtime value: "+v.getClass().getName());
        Map<String,Object> r=new LinkedHashMap<>();
        var methods=java.util.Arrays.stream(v.getClass().getMethods()).filter(m->java.lang.reflect.Modifier.isPublic(m.getModifiers())&&!java.lang.reflect.Modifier.isStatic(m.getModifiers())&&m.getParameterCount()==0&&(m.getName().startsWith("get")||m.getName().startsWith("is"))&&!m.getName().equals("getClass")&&!m.getName().equals("getCellsByLinearIndex")&&!m.getName().equals("getPartById")).sorted(java.util.Comparator.comparing(java.lang.reflect.Method::getName)).toList();
        for(var m:methods)r.put(m.getName(),graph(m.invoke(v),depth+1));
        return r;
    }
    private static List<String> ids(List<?> rows) throws Exception {List<String> out=new ArrayList<>();for(Object row:rows)out.add((String)row.getClass().getMethod("getId").invoke(row));return out;}
    private static Map<String,Object> binding(MocDocument doc,int index) throws Exception {
        Map<String,Object> out=(Map<String,Object>)graph(doc.keyformBinding(index),0);out.remove("getIndex");
        List<String> params=ids(doc.getParameters());
        for(Object row:(List<?>)out.get("getAxes")){Map<String,Object> axis=(Map<String,Object>)row;axis.put("parameter_id",params.get(((Number)axis.remove("getParameterIndex")).intValue()));}
        return out;
    }
    public static void main(String[] args)throws Exception{
        Map<String,String> o=new LinkedHashMap<>();
        for(int i=0;i<args.length;i+=2)o.put(args[i],args[i+1]);
        MocDocument doc=Moc3.INSTANCE.read(Files.readAllBytes(Path.of(o.get("--moc"))));
        List<String> want=Arrays.asList(o.get("--ids").split(","));
        List<String> meshes=ids(doc.getArtMeshes());
        List<Object> out=new ArrayList<>();
        for(String id:want){
            int index=meshes.indexOf(id);
            if(index<0)throw new IllegalStateException("Mesh not in MOC: "+id);
            Map<String,Object> b=binding(doc,index);
            Map<String,Object> row=new LinkedHashMap<>();
            Map<String,Object> mesh=(Map<String,Object>)graph(doc.getArtMeshes().get(index),0);
            Object ppi=mesh.remove("getParentPartIndex");
            if(ppi instanceof Number n){List<String> parts=ids(doc.getParts());row.put("parent_part",parts.get(n.intValue()));}
            Object pdi=mesh.remove("getParentDeformerIndex");
            if(pdi instanceof Number n){List<String> defs=ids(doc.getDeformers());row.put("parent_deformer",defs.get(n.intValue()));}
            row.put("draw_order",mesh.remove("getDrawOrder"));
            row.put("opacity",mesh.remove("getOpacity"));
            row.put("vertex_count",mesh.remove("getVertexCount"));
            row.put("texture_index",mesh.remove("getTextureIndex"));
            row.put("binding",b);
            out.add(row);
        }
        Files.writeString(Path.of(o.get("--output")),json(out));
        System.out.println("MOC_BINDING_OK "+o.get("--output"));
    }
}
