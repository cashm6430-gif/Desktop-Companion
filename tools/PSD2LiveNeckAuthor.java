import io.github.psd2live.agent.AgentProjectSnapshot;
import io.github.psd2live.agent.ViewModelAgentWorkspace;
import io.github.psd2live.project.ProjectArchive;
import io.github.psd2live.project.ProjectSession;
import io.github.psd2live.ui.state.PSD2LiveViewModel;
import io.github.psd2live.ui.state.PSD2LiveState;
import io.github.psd2live.core.Cmo3ImportMode;
import io.github.psd2live.core.Cmo3ModelImport;
import org.umamo.runtime.model.*;
import org.umamo.render.Moc3RestMeshKt;
import io.github.psd2live.core.*;
import org.umamo.interop.cmo3.Cmo3Conversion;
import org.umamo.format.cmo3.Cmo3;
import org.umamo.interop.moc3.export.Moc3Export;
import org.umamo.interop.moc3.Moc3ExportOptions;
import org.umamo.format.moc3.moc.MocVersion;
import kotlinx.serialization.json.*;
import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Comparator;
import kotlin.ResultKt;
import kotlin.Unit;
import kotlin.coroutines.Continuation;
import kotlin.coroutines.CoroutineContext;
import kotlin.coroutines.EmptyCoroutineContext;
import kotlin.coroutines.intrinsics.IntrinsicsKt;

import java.io.InputStream;
import java.net.URI;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/**
 * Standalone public-API neck authoring helper, pinned to installed PSD2Live 2.0.2.
 * Builds a one-layer PSD mesh/atlas, replaces its public PuppetModel mesh and
 * keyform graph, and calls public CMO3/MOC3 conversion. No Editor, MCP, private
 * reflection, original author workspace, or whole-character migration.
 * Public data-class copy/component methods with Kotlin value-class names are
 * invoked through getMethods(); access controls remain enabled throughout.
 *
 * --input requires a copied single-layer PSD, full 1254x1254 canvas.
 * --design JSON: vertexSourcePoints:[[x,y],...], indices:[triangle indices...].
 * Pixel X points right; pixel Y points down. ParamNeckBottomY is positive upward.
 * PPU is fixed at1254. BottomXY are source-pixel displacements; their MOC delta
 * is weight/1254. Curve adds2px horizontal middle bend at+1, endpoints fixed.
 * All outputs, author CMO, exact vertex/source order and hashes are isolated.
 * No output is automatically marked visually approved or promoted to assets.
 */
public final class PSD2LiveNeckAuthor {
    public static final String EXPECTED_JAR_NAME =
        "psd2live-2.0.2-2ad13515cfa82896b5e2819475e01d.jar";
    public static final String EXPECTED_JAR_SHA256 =
        "f1e1663887a6ceefd57dd5cfa25f09b958fb45fbe5a0aa27a72e866bf7b5e3d4";

    private interface SuspendCall {
        Object invoke(Continuation<Object> continuation) throws Throwable;
    }

    /** Kotlin Result is an unboxed success value or a Result.Failure object. */
    private static Object await(SuspendCall operation, long deadline) throws Throwable {
        CompletableFuture<Object> result = new CompletableFuture<>();
        Continuation<Object> continuation = new Continuation<>() {
            public CoroutineContext getContext() { return EmptyCoroutineContext.INSTANCE; }
            public void resumeWith(Object value) {
                try {
                    ResultKt.throwOnFailure(value);
                    result.complete(value);
                } catch (Throwable failure) {
                    result.completeExceptionally(failure);
                }
            }
        };
        Object initial = operation.invoke(continuation);
        if (initial != IntrinsicsKt.getCOROUTINE_SUSPENDED()) {
            ResultKt.throwOnFailure(initial);
            result.complete(initial);
        }
        long remaining = deadline - System.nanoTime();
        if (remaining <= 0) throw new TimeoutException("PSD2Live library deadline expired");
        try {
            return result.get(remaining, TimeUnit.NANOSECONDS);
        } catch (ExecutionException failure) {
            throw failure.getCause();
        }
    }

    private static Path absolute(String value, String label) {
        Path path = Path.of(value);
        if (!path.isAbsolute()) throw new IllegalArgumentException(label + " must be absolute");
        return path.normalize();
    }

    /** Resolve existing ancestors as well as the leaf, including Windows junctions. */
    private static Path target(String value, String label) throws Exception {
        Path requested = absolute(value, label);
        Path existing = requested;
        List<Path> suffix = new ArrayList<>();
        while (!Files.exists(existing, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            suffix.add(existing.getFileName());
            existing = existing.getParent();
            if (existing == null) throw new IllegalArgumentException("No existing ancestor: " + requested);
        }
        Path resolved = existing.toRealPath();
        for (int index = suffix.size() - 1; index >= 0; index--) resolved = resolved.resolve(suffix.get(index));
        return resolved.normalize();
    }

    private static boolean overlap(Path a, Path b) {
        return a.startsWith(b) || b.startsWith(a);
    }

    private static void emptyDirectory(Path path) throws Exception {
        if (Files.isSymbolicLink(path)) throw new IllegalArgumentException("Directory is a symlink: " + path);
        if (Files.exists(path)) {
            if (!Files.isDirectory(path)) throw new IllegalArgumentException("Not a directory: " + path);
            try (var entries = Files.list(path)) {
                if (entries.findAny().isPresent()) throw new IllegalArgumentException("Directory is not empty: " + path);
            }
        }
    }

    private static String sha256(Path path) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = Files.newInputStream(path)) {
            byte[] buffer = new byte[1024 * 1024];
            int length;
            while ((length = input.read(buffer)) != -1) digest.update(buffer, 0, length);
        }
        return HexFormat.of().formatHex(digest.digest());
    }

    private static Map<String, String> arguments(String[] values) {
        Map<String, String> result = new LinkedHashMap<>();
        List<String> allowed = List.of("--install", "--input", "--output", "--store", "--timeout-seconds", "--design");
        for (int index = 0; index < values.length; index += 2) {
            String name = values[index];
            if (!allowed.contains(name) || index + 1 >= values.length || result.containsKey(name))
                throw new IllegalArgumentException("Expected unique option/value pairs: " + allowed);
            result.put(name, values[index + 1]);
        }
        for (String name : allowed.subList(0, 4)) {
            if (!result.containsKey(name)) throw new IllegalArgumentException("Missing " + name);
        }
        return result;
    }

    private static String quote(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 32) out.append(String.format("\\u%04x", (int)c));
            else out.append(c);
        }
        return out.append('"').toString();
    }

    private static String json(Object value) {
        if (value == null) return "null";
        if (value instanceof Number || value instanceof Boolean) return value.toString();
        if (value instanceof Map<?, ?> map) {
            List<String> fields = new ArrayList<>();
            for (var item : map.entrySet()) fields.add(quote(item.getKey().toString()) + ":" + json(item.getValue()));
            return "{" + String.join(",", fields) + "}";
        }
        if (value instanceof List<?> list) {
            List<String> items = new ArrayList<>();
            for (Object item : list) items.add(json(item));
            return "[" + String.join(",", items) + "]";
        }
        return quote(value.toString());
    }

    private static AgentProjectSnapshot waitImported(PSD2LiveViewModel vm, ViewModelAgentWorkspace ws,
            long deadline) throws Exception {
        while (System.nanoTime() < deadline) {
            PSD2LiveState state = vm.getState().getValue();
            if (state.getErrorMessage() != null)
                throw new IllegalStateException("CMO3 import failed: " + state.getErrorMessage());
            AgentProjectSnapshot snap = ws.snapshot();
            if (snap.getLoaded() && !snap.getBusy() && snap.getHistoryHeadNodeId() != null
                    && snap.getPersistenceError() == null && state.getPreviewModel() != null)
                return snap;
            if (snap.getPersistenceError() != null)
                throw new IllegalStateException("CMO3 import persistence failed: " + snap.getPersistenceError());
            Thread.sleep(40);
        }
        throw new TimeoutException("CMO3 import never reached loaded/idle/authored-HEAD state");
    }

    private static Object modelGraph(PuppetModel puppet) throws Exception {
        return publicGraph(puppet, 0);
    }

    /** Public getters only: this diagnostic never bypasses Java access controls. */
    private static Object publicGraph(Object value, int depth) throws Exception {
        if (value == null || value instanceof String || value instanceof Number || value instanceof Boolean)
            return value;
        if (depth > 32) throw new IllegalStateException("Public model graph exceeds depth limit");
        if (value instanceof Enum<?> item) return item.name();
        if (value.getClass().isArray()) {
            List<Object> items = new ArrayList<>();
            for (int i = 0; i < Array.getLength(value); i++) items.add(publicGraph(Array.get(value, i), depth + 1));
            return items;
        }
        if (value instanceof Iterable<?> iterable) {
            List<Object> items = new ArrayList<>();
            for (Object item : iterable) items.add(publicGraph(item, depth + 1));
            return items;
        }
        if (value instanceof Map<?, ?> map) {
            Map<String, Object> items = new java.util.TreeMap<>();
            for (var item : map.entrySet()) items.put(item.getKey().toString(), publicGraph(item.getValue(), depth + 1));
            return items;
        }
        if (!value.getClass().getName().startsWith("org.umamo.runtime.model."))
            throw new IllegalStateException("Unmeasured public-model value type: " + value.getClass().getName());
        Map<String, Object> fields = new LinkedHashMap<>();
        fields.put("type", value.getClass().getName());
        List<String> excluded = List.of("getClass", "getPartById", "getCellsByLinearIndex");
        List<Method> getters = java.util.Arrays.stream(value.getClass().getMethods())
            .filter(method -> Modifier.isPublic(method.getModifiers()) && !Modifier.isStatic(method.getModifiers())
                && method.getParameterCount() == 0 && (method.getName().startsWith("get") || method.getName().startsWith("is"))
                && !excluded.contains(method.getName()))
            .sorted(Comparator.comparing(Method::getName)).toList();
        if (getters.isEmpty() && List.of("org.umamo.runtime.model.ParameterLabelColor$None",
                "org.umamo.runtime.model.PartGroupMode$PassThrough").contains(value.getClass().getName()))
            return fields; // Public sealed singleton has no state to compare.
        if (getters.isEmpty()) throw new IllegalStateException("Unmeasured runtime-model value: " + value.getClass().getName());
        for (Method getter : getters) {
            try {
                fields.put(getter.getName(), publicGraph(getter.invoke(value), depth + 1));
            } catch (java.lang.reflect.InvocationTargetException failure) {
                throw new IllegalStateException("Public getter failed: " + getter, failure.getCause());
            }
        }
        return fields;
    }

    private static Object publicCopy(Object source, Map<Integer, Object> changes) throws Exception {
        Method copy = java.util.Arrays.stream(source.getClass().getMethods())
            .filter(m -> Modifier.isPublic(m.getModifiers()) && !Modifier.isStatic(m.getModifiers())
                && (m.getName().equals("copy") || m.getName().startsWith("copy-")))
            .findFirst().orElseThrow(() -> new IllegalStateException("No public model copy API"));
        Object[] args = new Object[copy.getParameterCount()];
        for (int i = 0; i < args.length; i++) {
            final int component = i + 1;
            Method getter = java.util.Arrays.stream(source.getClass().getMethods())
                .filter(m -> m.getParameterCount() == 0 && (m.getName().equals("component" + component)
                    || m.getName().startsWith("component" + component + "-")))
                .findFirst().orElseThrow(() -> new IllegalStateException("No public component " + component));
            args[i] = changes.containsKey(component) ? changes.get(component) : getter.invoke(source);
        }
        return copy.invoke(source, args);
    }

    private static String publicId(Object source) throws Exception {
        Method getter = java.util.Arrays.stream(source.getClass().getMethods())
            .filter(m -> m.getParameterCount() == 0 && m.getName().startsWith("getId-"))
            .findFirst().orElseThrow();
        return (String)getter.invoke(source);
    }

    private static Object publicValueConstructor(Class<?> type, Object... arguments) throws Exception {
        // Kotlin hides value-class constructors from javac; the JVM wrapper is public.
        var constructor = java.util.Arrays.stream(type.getConstructors())
            .filter(c -> c.getParameterCount() == arguments.length + 1
                && c.getParameterTypes()[arguments.length] == kotlin.jvm.internal.DefaultConstructorMarker.class)
            .findFirst().orElseThrow(() -> new IllegalStateException("No public value constructor: " + type));
        Object[] supplied = java.util.Arrays.copyOf(arguments, arguments.length + 1);
        return constructor.newInstance(supplied);
    }

    private static float number(JsonElement element) {
        return Float.parseFloat(((JsonPrimitive)element).getContent());
    }

    private static float[] affine(float[] positions, float[] uv, int component) {
        double meanP = 0, meanU = 0;
        int n = positions.length / 2;
        for (int i = 0; i < n; i++) { meanP += positions[2*i+component]; meanU += uv[2*i+component]; }
        meanP /= n; meanU /= n;
        double numerator = 0, denominator = 0;
        for (int i = 0; i < n; i++) {
            double p = positions[2*i+component] - meanP;
            numerator += p * (uv[2*i+component] - meanU); denominator += p*p;
        }
        if (denominator == 0) throw new IllegalStateException("Degenerate seed atlas mapping");
        float scale = (float)(numerator / denominator), offset = (float)(meanU - scale * meanP);
        for (int i = 0; i < n; i++) {
            if (Math.abs(positions[2*i+component]*scale+offset-uv[2*i+component]) > 0.00001f)
                throw new IllegalStateException("Seed atlas mapping is not a verified affine UV mapping");
        }
        return new float[]{scale, offset};
    }

    private static void execute(Map<String, String> options, long deadline) throws Throwable {
        Path install = absolute(options.get("--install"), "--install").toRealPath();
        Path input = absolute(options.get("--input"), "--input").toRealPath();
        Path designPath = absolute(options.get("--design"), "--design").toRealPath();
        Path output = target(options.get("--output"), "--output");
        Path store = target(options.get("--store"), "--store");
        if (!input.toString().toLowerCase().endsWith(".psd")) throw new IllegalArgumentException("Expected copied PSD");
        if (overlap(output, store) || overlap(output, install) || overlap(store, install)
                || input.startsWith(output) || input.startsWith(store) || designPath.startsWith(output) || designPath.startsWith(store))
            throw new IllegalArgumentException("Isolation paths overlap");
        String defaultRoot = System.getenv("LOCALAPPDATA");
        Path originalStore = defaultRoot == null ? Path.of(System.getProperty("user.home"), ".psd2live", "agent-workspaces")
            : Path.of(defaultRoot, "PSD2Live", "agent-workspaces");
        if (overlap(store, target(originalStore.toAbsolutePath().toString(), "default store"))
                || overlap(output, target(originalStore.toAbsolutePath().toString(), "default store")))
            throw new IllegalArgumentException("Refusing original authoring store");
        String property = System.getProperty("psd2live.agent.store");
        if (property == null || !store.equals(target(property, "store property"))) throw new IllegalArgumentException("Missing isolated store property");
        emptyDirectory(output); emptyDirectory(store);
        Path jar = install.resolve("app").resolve(EXPECTED_JAR_NAME);
        String jarSha = sha256(jar), inputSha = sha256(input), designSha = sha256(designPath);
        if (!jarSha.equals(EXPECTED_JAR_SHA256)) throw new IllegalStateException("Installed API JAR changed: " + jarSha);
        System.setProperty("compose.application.resources.dir", install.resolve("app/resources").toString());
        System.setProperty("skiko.library.path", install.resolve("app").toString());
        System.setProperty("jpackage.app-version", "2.0.2");
        if (!Path.of(PSD2LivePipeline.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath().equals(jar.toRealPath()))
            throw new IllegalStateException("Different API JAR loaded");
        Files.createDirectories(store); Files.createDirectories(output);
        JsonObject design = (JsonObject)Json.Default.parseToJsonElement(Files.readString(designPath));
        JsonArray pointJson = (JsonArray)design.get("vertexSourcePoints");
        JsonArray indexJson = (JsonArray)design.get("indices");
        float[] positions = new float[pointJson.size()*2];
        List<List<Float>> points = new ArrayList<>();
        for (int i=0; i<pointJson.size(); i++) {
            JsonArray point = (JsonArray)pointJson.get(i);
            float x=number(point.get(0)), y=number(point.get(1));
            if (!Float.isFinite(x) || !Float.isFinite(y) || x<0 || x>1254 || y<0 || y>1254)
                throw new IllegalArgumentException("Invalid source point");
            positions[2*i]=x; positions[2*i+1]=y; points.add(List.of(x,y));
        }
        int[] indices = new int[indexJson.size()];
        if (indices.length%3!=0 || positions.length<6) throw new IllegalArgumentException("Invalid triangle mesh");
        for (int i=0; i<indices.length; i++) {
            indices[i]=Integer.parseInt(((JsonPrimitive)indexJson.get(i)).getContent());
            if (indices[i]<0 || indices[i]>=points.size()) throw new IllegalArgumentException("Invalid triangle index");
        }
        PipelineConfig config = (PipelineConfig)publicCopy(new PipelineConfig(), Map.of(
            1, 512, 18, true, 19, false, 26, false, 33, false, 36, false, 49, 1254f));
        System.out.println("BUILD_ISOLATED_NECK " + input);
        RigPreviewModel preview = new PSD2LivePipeline().buildPreview(input, config);
        PuppetModel seed = Moc3RestMeshKt.restMeshesToCanvasSpace(preview.getRig().getPuppet(), Map.of());
        if (seed.getDrawables().size()!=1 || !seed.getDeformers().isEmpty()
                || seed.getCanvasWidth()!=1254f || seed.getCanvasHeight()!=1254f)
            throw new IllegalStateException("Expected one mesh, zero deformers, 1254 canvas; got "
                +seed.getDrawables().size()+"/"+seed.getDeformers().size()+"/"+seed.getCanvasWidth()+"x"+seed.getCanvasHeight());
        Drawable original=seed.getDrawables().getFirst();
        String drawableId=publicId(original);
        float[] uAffine=affine(original.getMesh().getPositions(),original.getMesh().getUvs(),0);
        float[] vAffine=affine(original.getMesh().getPositions(),original.getMesh().getUvs(),1);
        float[] uv=new float[positions.length];
        for(int i=0;i<points.size();i++) {
            uv[2*i]=positions[2*i]*uAffine[0]+uAffine[1]; uv[2*i+1]=positions[2*i+1]*vAffine[0]+vAffine[1];
            if(uv[2*i]<0 || uv[2*i]>1 || uv[2*i+1]<0 || uv[2*i+1]>1) throw new IllegalStateException("Mesh outside atlas");
        }
        String[] names={"ParamNeckBottomX","ParamNeckBottomY","ParamNeckCurve"};
        float[][] keys={{-120f,0f,120f},{-120f,0f,120f},{-1f,0f,1f}};
        List<Parameter> parameters=new ArrayList<>(); List<KeyformAxis> axes=new ArrayList<>();
        ParameterKind normal=ParameterKind.valueOf("NORMAL");
        for(int a=0;a<3;a++) {
            parameters.add((Parameter)publicValueConstructor(Parameter.class, names[a], names[a], keys[a][0], keys[a][2],0f,normal,false,
                List.of(keys[a][0],keys[a][1],keys[a][2])));
            axes.add((KeyformAxis)publicValueConstructor(KeyformAxis.class,names[a],keys[a]));
        }
        List<KeyformCell<MeshDeltaForm>> cells=new ArrayList<>();
        for(int x=0;x<3;x++)for(int y=0;y<3;y++)for(int curve=0;curve<3;curve++) {
            float[] deltas=new float[positions.length];
            for(int i=0;i<points.size();i++) {
                float t=Math.max(0f,Math.min(1f,(positions[2*i+1]-570f)/18f));
                float weight=t*t*(3f-2f*t), curveWeight=4f*t*(1f-t);
                deltas[2*i]=weight*keys[0][x]+2f*curveWeight*keys[2][curve];
                deltas[2*i+1]=-weight*keys[1][y];
            }
            cells.add(new KeyformCell<>(new int[]{x,y,curve},new MeshDeltaForm(deltas)));
        }
        Drawable neck=(Drawable)publicCopy(original,Map.of(6,new DrawableMesh(positions,uv,indices),
            7,new KeyformGrid<>(axes,cells),8,new ChannelGrids(Map.of())));
        PuppetModel model=(PuppetModel)publicCopy(seed,Map.of(1,parameters,4,List.of(neck),9,List.of(),10,List.of(),15,1254f));
        Files.writeString(output.resolve("neck-public-graph.json"),json(modelGraph(model))+"\n");
        // No 5.3-only blend/offscreen features: use the tested 5.0 MOC writer.
        var moc=Moc3Export.INSTANCE.write(model,MocVersion.V50,
            Moc3RestMeshKt.canvasToParentSpaceFor(model,Map.of()),new Moc3ExportOptions(false,false,false,false,false,true,1254f));
        Path mocPath=output.resolve("neck-surface.moc3"); Files.write(mocPath,moc.getFirst());
        List<Cmo3Conversion.AtlasPage> atlas=new ArrayList<>();
        for(int i=0;i<preview.getAtlas().getPages().size();i++) {
            io.github.psd2live.core.AtlasPage page=preview.getAtlas().getPages().get(i);
            Files.write(output.resolve("texture_"+i+".png"),page.getPng());
            atlas.add(new Cmo3Conversion.AtlasPage(page.getPng(),page.getImage().getWidth(),page.getImage().getHeight()));
        }
        var cmo=Cmo3Conversion.INSTANCE.freshCmo3(model,atlas,Map.of(drawableId,0),"Neck surface",0L,30,tile -> null,null);
        Path cmoPath=output.resolve("neck-surface.cmo3"); Files.write(cmoPath,Cmo3.INSTANCE.write(cmo.getModel()));
        Map<String,Object> refs=new LinkedHashMap<>();
        refs.put("Version",3);refs.put("FileReferences",Map.of("Moc","neck-surface.moc3","Textures",List.of("texture_0.png")));
        Files.writeString(output.resolve("neck-surface.model3.json"),json(refs)+"\n");
        Map<String,Object> rig=new LinkedHashMap<>();
        rig.put("version",1);rig.put("drawable",drawableId);rig.put("vertexSourcePoints",points);
        rig.put("parameters",Map.of("bottomX",names[0],"bottomY",names[1],"curve",names[2]));
        rig.put("pixelsPerUnit",1254);rig.put("sourceCanvasSize",List.of(1254,1254));
        rig.put("bottomWeightContract","smoothstep(clamp((sourceY-570)/18,0,1))");
        rig.put("parameterDomain",Map.of(names[0],List.of(-120,120),names[1],List.of(-120,120),names[2],List.of(-1,1)));
        rig.put("axisConvention","X source-pixels right, Y source-pixels Native up; geometry-grid pixels before MOC scaling");
        Files.writeString(output.resolve("neck-surface.rig.json"),json(rig)+"\n");
        if(!sha256(input).equals(inputSha) || !sha256(designPath).equals(designSha) || !sha256(jar).equals(jarSha))
            throw new IllegalStateException("Input/JAR changed during isolated authoring");
        Map<String,String> hashes=new LinkedHashMap<>();
        try(var paths=Files.list(output)) {for(Path path:paths.filter(Files::isRegularFile).sorted().toList())hashes.put(path.getFileName().toString(),sha256(path));}
        Map<String,Object> report=new LinkedHashMap<>();
        report.put("version",1);report.put("status","exported_core_verification_pending_visual_unreviewed");
        report.put("mainJarSha256",jarSha);report.put("inputSha256",inputSha);report.put("designSha256",designSha);
        report.put("drawable",drawableId);report.put("vertices",points.size());report.put("triangles",indices.length/3);
        report.put("outputSha256",hashes);report.put("createdUtc",Instant.now().toString());
        Files.writeString(output.resolve("neck-authoring-report.json"),json(report)+"\n");
        System.out.println("EXPORTED_REAL_PARAMETER_NECK "+points.size()+" vertices "+indices.length/3+" triangles "+drawableId);
    }

    public static void main(String[] args) {
        try {
            Map<String, String> options = arguments(args);
            int seconds = Integer.parseInt(options.getOrDefault("--timeout-seconds", "120"));
            if (seconds < 1 || seconds > 600) throw new IllegalArgumentException("Timeout must be 1..600 seconds");
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(seconds);
            CompletableFuture<Void> completed = new CompletableFuture<>();
            Thread worker = new Thread(() -> {
                try { execute(options, deadline); completed.complete(null); }
                catch (Throwable failure) { completed.completeExceptionally(failure); }
            }, "psd2live-isolated-library-roundtrip");
            worker.setDaemon(true);
            worker.start();
            try { completed.get(seconds, TimeUnit.SECONDS); }
            catch (ExecutionException failure) { throw failure.getCause(); }
            System.exit(0); // Dispose remaining AWT/native threads of this helper only.
        } catch (Throwable failure) {
            System.err.println("LIBRARY_ROUNDTRIP_FAILED " + failure.getClass().getName() + ": " + failure.getMessage());
            failure.printStackTrace(System.err);
            System.exit(1);
        }
    }
}
