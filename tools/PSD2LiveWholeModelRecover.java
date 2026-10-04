import org.umamo.format.moc3.Moc3;
import org.umamo.format.moc3.MocDocument;
import org.umamo.format.moc3.json.Cdi3Json;
import org.umamo.format.cmo3.Cmo3;
import org.umamo.interop.cmo3.Cmo3Conversion;
import org.umamo.interop.moc3.Moc3ExportOptions;
import org.umamo.interop.moc3.export.Moc3Export;
import org.umamo.runtime.model.PuppetModel;
import org.umamo.runtime.model.Deformer;
import org.umamo.runtime.model.Drawable;
import org.umamo.runtime.model.KeyformGrid;
import org.umamo.runtime.model.KeyformCell;
import org.umamo.runtime.model.WarpLatticeForm;
import io.github.psd2live.core.Cmo3ModelImport;
import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Standalone public-API author recovery, locked to PSD2Live 2.0.2 and the current
 * formal MOC. No GUI, ViewModel, MCP, private access, original-store mutation,
 * runtime correction/cache, remeshing, or atlas repacking.
 *
 * Required option/value pairs: --install --moc --display --atlas --output --store.
 * Optional flag: --dump-diagnostics (large author/MOC graphs in this fresh run).
 * Compile: javac -encoding UTF-8 -cp "D:/tool/psd2live/app/*" -d <fresh classes>
 *          PSD2LiveWholeModelRecover.java
 * Run: java -Dpsd2live.agent.store=<fresh store> -Djava.io.tmpdir=<fresh temp>
 *      -cp "<classes>;D:/tool/psd2live/app/*" PSD2LiveWholeModelRecover <options>
 * Create the isolated temp directory before starting Java (ImageIO cache).
 *
 * Preserve parent-local rest meshes; do not invoke restMeshesToCanvasSpace.
 * Public MOC import controls are SOURCE PIXELS only at the root warp. Three
 * right-column DeformBodyShift CPs need nextUp(source-float) to preserve the
 * original Native float through inverse PPU conversion. The native ranges and
 * all other forms remain unchanged, including Gape 1..2 (runtime logical 0..1
 * remains a separate existing CubismCanvas contract).
 *
 * The recovered CMO uses workflow-pinned original atlas mode and embedded per-drawable
 * atlas crops, not the original PSD layer provenance/history. Keep the real
 * 34-layer PSD/PNG package separately. This tool never silently relinks/repaints
 * those sources or changes source mode; geometry edits keep the same UVs/page.
 * Actual Cubism Editor GUI appearance and Qt pixel/scene equivalence are gates
 * outside this helper. A full formal runtime family/metadata is required for Qt.
 */
public final class PSD2LiveWholeModelRecover {
    private static final String JAR_NAME="psd2live-2.0.2-2ad13515cfa82896b5e2819475e01d.jar";
    private static final String JAR_SHA="f1e1663887a6ceefd57dd5cfa25f09b958fb45fbe5a0aa27a72e866bf7b5e3d4";
    private static final String MOC_SHA="df5e86d822959864ba9f07b1967b792ebb17d9407a83c77b2243575a8027c931";
    private static final String DISPLAY_SHA="b11ad69f3de3df80f33efa9fe82172a3052eef639903a3bb6c17d855dea7ff2f";
    private static final String ATLAS_SHA="e27d3aac6faa2eb0cbfc8e3275ccc57ec563aeaa685cd53a4a0cd5151ad6b90e";

    private static String sha(Path path) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(path)));
    }
    private static String quote(String s) {
        StringBuilder out=new StringBuilder("\"");
        for(char c:s.toCharArray()) {
            if(c=='"'||c=='\\') out.append('\\').append(c);
            else if(c<32) out.append(String.format("\\u%04x",(int)c));
            else out.append(c);
        }
        return out.append('"').toString();
    }
    private static String json(Object v) {
        if(v==null) return "null";
        if(v instanceof Number || v instanceof Boolean) return v.toString();
        if(v instanceof Map<?,?> m) {
            List<String> r=new ArrayList<>();
            for(var e:m.entrySet()) r.add(quote(e.getKey().toString())+":"+json(e.getValue()));
            return "{"+String.join(",",r)+"}";
        }
        if(v instanceof List<?> l) {
            List<String> r=new ArrayList<>(); for(var e:l) r.add(json(e));
            return "["+String.join(",",r)+"]";
        }
        return quote(v.toString());
    }
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
    private static void empty(Path p) throws Exception {
        if(!p.isAbsolute()) throw new IllegalArgumentException("Absolute path required");
        if(Files.exists(p)) try(var ls=Files.list(p)) {
            if(ls.findAny().isPresent()) throw new IllegalArgumentException("Fresh directory required: "+p);
        }
        Files.createDirectories(p);
    }
    /** Kotlin value-class copy names are mangled, but these are public methods. */
    private static Object publicCopy(Object source,Map<Integer,Object> changes) throws Exception {
        Method copy=java.util.Arrays.stream(source.getClass().getMethods())
            .filter(m -> Modifier.isPublic(m.getModifiers())&&!Modifier.isStatic(m.getModifiers())
                &&m.getName().startsWith("copy-")&&!m.getName().contains("$"))
            .findFirst().orElseThrow();
        Object[] args=new Object[copy.getParameterCount()];
        for(int i=0;i<args.length;i++) {
            final int component=i+1;
            Method getter=java.util.Arrays.stream(source.getClass().getMethods()).filter(m -> m.getParameterCount()==0
                &&(m.getName().equals("component"+component)||m.getName().startsWith("component"+component+"-")))
                .findFirst().orElseThrow();
            args[i]=changes.containsKey(component)?changes.get(component):getter.invoke(source);
        }
        return copy.invoke(source,args);
    }
    private static PuppetModel recoverShiftRounding(PuppetModel source) throws Exception {
        List<Deformer> ds=new ArrayList<>();int changed=0;
        for(Deformer d:source.getDeformers()) {
            String id=(String)d.getClass().getMethod("getId-u_t8HeU").invoke(d);
            if(!id.equals("DeformBodyShift")){ds.add(d);continue;}
            if(!(d instanceof Deformer.Warp warp))throw new IllegalStateException("BodyShift not warp");
            var grid=warp.getGeometryGrid();
            if(grid.getAxes().size()!=2||!java.util.Arrays.equals(grid.getAxes().get(0).getKeys(),new float[]{-100,0,100})
                ||!java.util.Arrays.equals(grid.getAxes().get(1).getKeys(),new float[]{-100,0,100}))
                throw new IllegalStateException("Unexpected BodyShift parameter lattice");
            List<KeyformCell<WarpLatticeForm>> cells=new ArrayList<>();
            for(var cell:grid.getCells()) {
                if(!java.util.Arrays.equals(cell.getCoordinate(),new int[]{2,1})){cells.add(cell);continue;}
                float[] cp=cell.getForm().getControlPoints().clone();
                if(cp.length!=18)throw new IllegalStateException("Unexpected BodyShift lattice size");
                for(int i:new int[]{4,10,16}) {
                    if(Float.floatToIntBits(cp[i])!=Float.floatToIntBits(1368.6072f))
                        throw new IllegalStateException("Unexpected original BodyShift control point");
                    // Public author coordinates are source pixels. One ULP here
                    // preserves the original MOC float through inverse scaling.
                    cp[i]=Math.nextUp(cp[i]);changed++;
                }
                cells.add(new KeyformCell<>(cell.getCoordinate(),new WarpLatticeForm(cp)));
            }
            ds.add((Deformer)publicCopy(warp,Map.of(8,new KeyformGrid<>(grid.getAxes(),cells))));
        }
        if(changed!=3)throw new IllegalStateException("Expected exactly three source-pixel rounding corrections");
        return (PuppetModel)publicCopy(source,Map.of(3,ds));
    }
    private static PuppetModel bindOriginalAtlas(PuppetModel source,Map<String,Integer> pages,float ppu) throws Exception {
        List<Drawable> draws=new ArrayList<>();
        for(Drawable d:source.getDrawables()) {
            String id=(String)d.getClass().getMethod("getId-bteaJEs").invoke(d);
            Integer page=pages.get(id);if(page==null)throw new IllegalStateException("Unbound original atlas mesh "+id);
            draws.add((Drawable)publicCopy(d,Map.of(19,page)));
        }
        return (PuppetModel)publicCopy(source,Map.of(4,draws,15,ppu));
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
    private static Map<String,String> options(String[] args) {
        List<String> allowed=List.of("--install","--moc","--display","--atlas","--output","--store");
        Map<String,String> out=new LinkedHashMap<>();
        for(int i=0;i<args.length;i++) {
            String key=args[i];if(out.containsKey(key))throw new IllegalArgumentException("Duplicate option");
            if(key.equals("--dump-diagnostics")){out.put(key,"true");continue;}
            if(!allowed.contains(key)||i+1>=args.length)throw new IllegalArgumentException("Expected "+allowed);
            out.put(key,args[++i]);
        }
        for(String key:allowed)if(!out.containsKey(key))throw new IllegalArgumentException("Missing "+key);
        return out;
    }
    private static List<String> ids(List<?> rows) throws Exception {
        List<String> out=new ArrayList<>();for(Object row:rows)out.add((String)row.getClass().getMethod("getId").invoke(row));return out;
    }
    @SuppressWarnings("unchecked")
    private static Map<String,Object> binding(MocDocument doc,int index) throws Exception {
        Map<String,Object> out=(Map<String,Object>)graph(doc.keyformBinding(index),0);out.remove("getIndex");
        List<String> params=ids(doc.getParameters());
        for(Object row:(List<?>)out.get("getAxes")) {
            Map<String,Object> axis=(Map<String,Object>)row;
            axis.put("parameter_id",params.get(((Number)axis.remove("getParameterIndex")).intValue()));
        }
        return out;
    }
    @SuppressWarnings("unchecked")
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
    private static Object semantics(MocDocument doc) throws Exception {
        if(!doc.getGlues().isEmpty()||!doc.getBlendShapes().isEmpty()||!doc.getOffscreens().isEmpty())
            throw new IllegalStateException("Unmeasured extra runtime feature");
        Map<String,Object> out=new java.util.TreeMap<>();
        out.put("version",doc.getVersion().name());out.put("parameter_union",doc.getKeyPositionsHasParameterUnion());
        out.put("canvas",graph(doc.getCanvas(),0));out.put("parameters",graph(doc.getParameters(),0));
        out.put("parts",named(doc,doc.getParts()));out.put("deformers",named(doc,doc.getDeformers()));out.put("meshes",named(doc,doc.getArtMeshes()));
        List<Object> bindings=new ArrayList<>();for(var b:doc.getBindings())bindings.add(binding(doc,b.getIndex()));
        bindings.sort(Comparator.comparing(PSD2LiveWholeModelRecover::json));out.put("bindings",bindings);
        Object groups=graph(doc.getRenderOrderGroups(),0);List<String> meshIds=ids(doc.getArtMeshes());
        for(Object value:(List<?>)groups)for(Object child:(List<?>)((Map<?,?>)value).get("getChildren")) {
            Map<String,Object> row=(Map<String,Object>)child;
            if(((Number)row.get("getKind")).intValue()!=0)throw new IllegalStateException("Unmeasured render child kind");
            row.put("drawable_id",meshIds.get(((Number)row.remove("getIndex")).intValue()));
        }
        out.put("render_groups",groups);return out;
    }
    private static String hashValue(Object value) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(json(value).getBytes(java.nio.charset.StandardCharsets.UTF_8)));
    }
    private static int deformerKeyforms(MocDocument doc) throws Exception {
        int count=0;for(Object d:doc.getDeformers())count+=((List<?>)d.getClass().getMethod("getKeyforms").invoke(d)).size();return count;
    }
    public static void main(String[] args) throws Exception {
        Map<String,String> opts=options(args);
        Path moc=target(opts.get("--moc")),cdi=target(opts.get("--display")),atlas=target(opts.get("--atlas"));
        Path out=target(opts.get("--output")),store=target(opts.get("--store")),install=target(opts.get("--install"));
        if(overlap(out,store)||overlap(out,install)||overlap(store,install))throw new IllegalArgumentException("Disjoint output/store/install required");
        for(Path input:List.of(moc,cdi,atlas))if(input.startsWith(out)||input.startsWith(store))throw new IllegalArgumentException("Output must be disjoint from sources");
        String env=System.getenv("LOCALAPPDATA");
        Path original=env==null?Path.of(System.getProperty("user.home"),".psd2live","agent-workspaces"):Path.of(env,"PSD2Live","agent-workspaces");
        original=target(original.toAbsolutePath().toString());
        if(overlap(out,original)||overlap(store,original))throw new IllegalArgumentException("Default author store forbidden");
        String property=System.getProperty("psd2live.agent.store");
        if(property==null||!target(property).equals(store))throw new IllegalArgumentException("Isolated store property required before JVM startup");
        empty(out);empty(store);
        Path jar=install.resolve("app").resolve(JAR_NAME);
        if(!sha(jar).equals(JAR_SHA))throw new IllegalStateException("Unexpected PSD2Live JAR SHA");
        if(!Path.of(Moc3.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath().equals(jar.toRealPath()))throw new IllegalStateException("Unexpected loaded JAR");
        if(!sha(moc).equals(MOC_SHA)||!sha(cdi).equals(DISPLAY_SHA)||!sha(atlas).equals(ATLAS_SHA))
            throw new IllegalArgumentException("Recovery is bound to the frozen current formal MOC/CDI/atlas family; audit another source explicitly");
        Map<String,String> inputs=Map.of("moc",sha(moc),"display",sha(cdi),"atlas",sha(atlas),"jar",sha(jar));
        MocDocument doc=Moc3.INSTANCE.read(Files.readAllBytes(moc));
        Class<?> importer=Class.forName("org.umamo.interop.moc3.import.Moc3Import");
        PuppetModel parent=(PuppetModel)importer.getMethod("fromMocDocument",MocDocument.class,Cdi3Json.class,boolean.class)
            .invoke(importer.getField("INSTANCE").get(null),doc,Moc3.INSTANCE.readCdi3(Files.readString(cdi)),false);
        PuppetModel corrected=recoverShiftRounding(parent);
        Map<String,Integer> pagesByMesh=new LinkedHashMap<>();for(var mesh:doc.getArtMeshes())pagesByMesh.put(mesh.getId(),mesh.getTextureIndex());
        byte[] png=Files.readAllBytes(atlas);var image=javax.imageio.ImageIO.read(new java.io.ByteArrayInputStream(png));
        var pages=List.of(new Cmo3Conversion.AtlasPage(png,image.getWidth(),image.getHeight()));
        var author=Cmo3Conversion.INSTANCE.freshCmo3(corrected,pages,pagesByMesh,"Whale girl formal runtime recovery",0L,30,tile -> null,null);
        byte[] cmo=Cmo3.INSTANCE.write(author.getModel());
        PuppetModel readback=Cmo3ModelImport.INSTANCE.read(cmo).getPuppet();
        readback=bindOriginalAtlas(readback,pagesByMesh,doc.getCanvas().getPixelsPerUnit());
        var nativeExport=Moc3Export.INSTANCE.write(readback,doc.getVersion(),null,
            new Moc3ExportOptions(true,true,true,false,false,true,doc.getCanvas().getPixelsPerUnit()));
        if(!nativeExport.getSecond().isEmpty())throw new IllegalStateException("Native author export notices: "+nativeExport.getSecond());
        MocDocument exported=Moc3.INSTANCE.read(nativeExport.getFirst());
        Object before=semantics(doc),after=semantics(exported);
        if(!before.equals(after))throw new IllegalStateException("All decoded runtime values are not exact; do not adopt");
        Files.write(out.resolve("recovered.cmo3"),cmo);Files.write(out.resolve("recovered.moc3"),nativeExport.getFirst());
        Files.write(out.resolve("texture_00.png"),png);Files.write(out.resolve("recovered.cdi3.json"),Files.readAllBytes(cdi));
        Files.writeString(out.resolve("recovered.model3.json"),"{\"Version\":3,\"FileReferences\":{\"Moc\":\"recovered.moc3\",\"Textures\":[\"texture_00.png\"],\"DisplayInfo\":\"recovered.cdi3.json\"}}\n");
        if(opts.containsKey("--dump-diagnostics")) {
            Path diagnostics=out.resolve("diagnostics");Files.createDirectory(diagnostics);
            Files.writeString(diagnostics.resolve("formal-decoded-runtime.json"),json(before)+"\n");
            Files.writeString(diagnostics.resolve("recovered-decoded-runtime.json"),json(after)+"\n");
            Files.writeString(diagnostics.resolve("author-public-graph.json"),json(graph(readback,0))+"\n");
        }
        Map<String,Object> report=new LinkedHashMap<>();
        report.put("schema_version",1);report.put("kind","version_locked_public_api_whole_model_recovery");
        report.put("source_sha256",inputs);report.put("public_api","Moc3.read -> Moc3Import.fromMocDocument(compact=false) -> public grid copy -> freshCmo3 -> Cmo3.write -> Cmo3ModelImport.read -> explicit original atlas/PPU binding -> Moc3Export.write(mapping=null)");
        report.put("isolated_store",store.toString());report.put("runtime_export_notices",nativeExport.getSecond().toString());
        report.put("cmo_notices",author.getReport().toString());
        report.put("author_source_mode","Original atlas-mode CMO with original PNG cache and per-drawable atlas crops; real 34 source layers/PSD stay in separately hashed author package.");
        report.put("parent_coordinate_contract","Keep imported parent-local mesh/grid coordinates. Root BodyShift warp is source pixels; its CPs 4/10/16 at cell[2,1] advance one source float32 ULP before export. Do not globally move rest meshes to canvas.");
        report.put("all_decoded_runtime_values_exact",true);report.put("canonical_runtime_before_sha256",hashValue(before));report.put("canonical_runtime_after_sha256",hashValue(after));
        report.put("counts",Map.of("parameters",parent.getParameters().size(),"parts",parent.getParts().size(),"deformers",parent.getDeformers().size(),"drawables",parent.getDrawables().size(),"bindings",doc.getBindings().size(),"mesh_keyforms",doc.getArtMeshes().stream().mapToInt(m -> m.getKeyforms().size()).sum(),"deformer_keyforms",deformerKeyforms(doc)));
        report.put("approval",Map.of("engineering","public author closed loop and all decoded runtime values exact","visual","pending","adoption","not_automatic"));
        Map<String,String> outputs=new LinkedHashMap<>();for(String name:List.of("recovered.cmo3","recovered.moc3","texture_00.png","recovered.cdi3.json","recovered.model3.json"))outputs.put(name,sha(out.resolve(name)));report.put("output_sha256",outputs);
        if(!inputs.equals(Map.of("moc",sha(moc),"display",sha(cdi),"atlas",sha(atlas),"jar",sha(jar))))throw new IllegalStateException("Sources or JAR changed during recovery");
        report.put("source_sha256_verified_after",true);Files.writeString(out.resolve("recovery-report.json"),json(report)+"\n");
        System.out.println("PUBLIC_RECOVERY_EXACT "+out.resolve("recovery-report.json"));
    }
}
