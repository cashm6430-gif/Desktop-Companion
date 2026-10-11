import io.github.psd2live.core.Cmo3ModelImport;
import org.umamo.runtime.model.*;
import org.umamo.render.Moc3RestMeshKt;
import org.umamo.interop.moc3.export.Moc3Export;
import org.umamo.interop.moc3.Moc3ExportOptions;
import org.umamo.format.moc3.moc.MocVersion;
import kotlin.Pair;
import kotlin.jvm.functions.Function2;
import kotlin.jvm.internal.DefaultConstructorMarker;

import java.io.InputStream;
import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Isolated, version-pinned public-API arm author for the whale-girl model.
 * Decodes an ISOLATED .cmo3 copy with the public Cmo3ModelImport API, applies a
 * declared mutation (identity by default), and re-exports a .moc3 with the
 * public Moc3Export API. No Editor/MCP, no private reflection, no setAccessible,
 * no original workspace. Textures are never touched.
 *
 * The first (identity) mode exists to PROVE the decode -> export -> render path
 * against the known-good round-trip before any geometry is authored: it emits
 * both plausible space strategies so exactly one can be confirmed by rendering.
 *
 * Usage:
 *   java -Dpsd2live.agent.store=<store> -Djava.io.tmpdir=<tmp> -cp "<classes>;app/*"
 *        PSD2LiveArmAuthor --install D:/tool/psd2live
 *        --input <isolated-copy.cmo3> --output <new-dir> --timeout-seconds 120
 */
public final class PSD2LiveArmAuthor {
    public static final String EXPECTED_JAR_NAME =
        "psd2live-2.0.2-2ad13515cfa82896b5e2819475e01d.jar";
    public static final String EXPECTED_JAR_SHA256 =
        "f1e1663887a6ceefd57dd5cfa25f09b958fb45fbe5a0aa27a72e866bf7b5e3d4";
    private static final float PPU = 1254f;

    private static Path absolute(String value, String label) {
        Path path = Path.of(value);
        if (!path.isAbsolute()) throw new IllegalArgumentException(label + " must be absolute");
        return path.normalize();
    }

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
        for (int i = suffix.size() - 1; i >= 0; i--) resolved = resolved.resolve(suffix.get(i));
        return resolved.normalize();
    }

    private static boolean overlap(Path a, Path b) { return a.startsWith(b) || b.startsWith(a); }

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

    private static Map<String, Object> arguments(String[] values) {
        Map<String, Object> result = new LinkedHashMap<>();
        List<String> singles = List.of("--install", "--input", "--output", "--store", "--timeout-seconds");
        List<String> allowed = new ArrayList<>(singles);
        allowed.add("--hide");
        allowed.add("--only");
        for (int i = 0; i < values.length; i += 2) {
            String name = values[i];
            if (!allowed.contains(name) || i + 1 >= values.length)
                throw new IllegalArgumentException("Expected option/value pairs: " + allowed);
            if (name.equals("--hide") || name.equals("--only")) {
                @SuppressWarnings("unchecked")
                List<String> list = (List<String>) result.computeIfAbsent(name, k -> new ArrayList<String>());
                list.add(values[i + 1]);
            } else {
                if (result.containsKey(name)) throw new IllegalArgumentException("Duplicate " + name);
                result.put(name, values[i + 1]);
            }
        }
        for (String name : singles) {
            if (!result.containsKey(name)) throw new IllegalArgumentException("Missing " + name);
        }
        return result;
    }

    private static String quote(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 32) out.append(String.format("\\u%04x", (int) c));
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

    // ---- public reflective data-class helpers (no access-control bypass) ----

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
        return (String) getter.invoke(source);
    }

    private static Object publicValueConstructor(Class<?> type, Object... arguments) throws Exception {
        var constructor = java.util.Arrays.stream(type.getConstructors())
            .filter(c -> c.getParameterCount() == arguments.length + 1
                && c.getParameterTypes()[arguments.length] == DefaultConstructorMarker.class)
            .findFirst().orElseThrow(() -> new IllegalStateException("No public value constructor: " + type));
        Object[] supplied = java.util.Arrays.copyOf(arguments, arguments.length + 1);
        return constructor.newInstance(supplied);
    }

    /** Public getters only; never setAccessible, fields, or mutators. */
    private static Object publicGraph(Object value, int depth) throws Exception {
        if (value == null || value instanceof String || value instanceof Number || value instanceof Boolean) return value;
        if (depth > 40) throw new IllegalStateException("Public model graph exceeds depth limit");
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
            .filter(m -> Modifier.isPublic(m.getModifiers()) && !Modifier.isStatic(m.getModifiers())
                && m.getParameterCount() == 0 && (m.getName().startsWith("get") || m.getName().startsWith("is"))
                && !excluded.contains(m.getName()))
            .sorted(Comparator.comparing(Method::getName)).toList();
        if (getters.isEmpty() && List.of("org.umamo.runtime.model.ParameterLabelColor$None",
                "org.umamo.runtime.model.PartGroupMode$PassThrough").contains(value.getClass().getName()))
            return fields;
        if (getters.isEmpty()) throw new IllegalStateException("Unmeasured runtime-model value: " + value.getClass().getName());
        for (Method getter : getters) {
            try { fields.put(getter.getName(), publicGraph(getter.invoke(value), depth + 1)); }
            catch (java.lang.reflect.InvocationTargetException failure) {
                throw new IllegalStateException("Public getter failed: " + getter, failure.getCause());
            }
        }
        return fields;
    }

    private static Moc3ExportOptions exportOptions() {
        // (exportHiddenParts, hiddenDrawables, guideImageParts, includePhysics,
        //  includeUserData, includeDisplayInfo, pixelsPerUnitOverride)
        return new Moc3ExportOptions(false, false, false, false, false, true, null);
    }

    @SuppressWarnings("unchecked")
    private static void execute(Map<String, Object> options) throws Exception {
        Path install = absolute((String) options.get("--install"), "--install").toRealPath();
        Path archive = absolute((String) options.get("--input"), "--input").toRealPath();
        Path output = target((String) options.get("--output"), "--output");
        Path store = target((String) options.get("--store"), "--store");
        List<String> hides = (List<String>) options.getOrDefault("--hide", List.of());
        List<String> onlys = (List<String>) options.getOrDefault("--only", List.of());
        if (!Files.isRegularFile(archive) || !archive.toString().toLowerCase().endsWith(".cmo3"))
            throw new IllegalArgumentException("Expected an existing isolated .cmo3 copy");
        if (overlap(output, store) || overlap(output, install) || overlap(store, install)
                || archive.startsWith(output) || archive.startsWith(store))
            throw new IllegalArgumentException("Output/store must be disjoint from installation and input");
        emptyDirectory(output);
        Path mainJar = install.resolve("app").resolve(EXPECTED_JAR_NAME);
        String jarSha = sha256(mainJar);
        if (!EXPECTED_JAR_SHA256.equals(jarSha))
            throw new IllegalStateException("Unsupported PSD2Live JAR SHA256: " + jarSha);
        String inputSha = sha256(archive);
        System.setProperty("compose.application.resources.dir", install.resolve("app/resources").toString());
        System.setProperty("skiko.library.path", install.resolve("app").toString());
        System.setProperty("jpackage.app-version", "2.0.2");
        Files.createDirectories(output);

        System.out.println("ARM_AUTHOR 2.0.2 " + jarSha);
        PuppetModel decoded = Cmo3ModelImport.INSTANCE.read(Files.readAllBytes(archive)).getPuppet();
        System.out.println("DECODED params=" + decoded.getParameters().size()
            + " drawables=" + decoded.getDrawables().size()
            + " deformers=" + decoded.getDeformers().size()
            + " canvas=" + decoded.getCanvasWidth() + "x" + decoded.getCanvasHeight()
            + " ppu=" + decoded.getPixelsPerUnit());
        Files.writeString(output.resolve("decoded-model-public-graph.json"),
            json(publicGraph(decoded, 0)) + "\n");

        PuppetModel model = decoded;
        List<String> hiddenNames = new ArrayList<>();
        if (!hides.isEmpty() || !onlys.isEmpty()) {
            List<Drawable> drawables = new ArrayList<>(decoded.getDrawables());
            for (int i = 0; i < drawables.size(); i++) {
                Drawable d = drawables.get(i);
                String n = d.getName() == null ? "" : d.getName().toLowerCase();
                boolean hideThis;
                if (!onlys.isEmpty()) {
                    hideThis = true;
                    for (String o : onlys) if (n.contains(o.toLowerCase())) { hideThis = false; break; }
                } else {
                    hideThis = false;
                    for (String h : hides) if (n.contains(h.toLowerCase())) { hideThis = true; break; }
                }
                if (hideThis) {
                    drawables.set(i, (Drawable) publicCopy(d, Map.of(16, false)));
                    hiddenNames.add(d.getName());
                    System.out.println("HIDE #" + i + " '" + d.getName() + "'");
                }
            }
            model = (PuppetModel) publicCopy(model, Map.of(4, drawables));
            System.out.println("HIDDEN_DRAWABLES " + hiddenNames.size());
        }

        Map<String, Object> report = new LinkedHashMap<>();
        report.put("schema_version", 1);
        report.put("kind", "public_api_arm_author_identity_probe");
        report.put("created_utc", Instant.now().toString());
        report.put("main_jar_sha256", jarSha);
        report.put("input_sha256", inputSha);
        report.put("canvas_width", decoded.getCanvasWidth());
        report.put("canvas_height", decoded.getCanvasHeight());
        report.put("pixels_per_unit", decoded.getPixelsPerUnit());
        report.put("hidden_drawables", hiddenNames);
        Map<String, Object> strategies = new LinkedHashMap<>();

        // Strategy A ("asis"): hand the decoded model straight to the writer.
        try {
            Function2<DrawableId, float[], float[]> space = Moc3RestMeshKt.canvasToParentSpaceFor(model, Map.of());
            Pair<byte[], ?> pair = Moc3Export.INSTANCE.write(model, MocVersion.V50, space, exportOptions());
            byte[] moc = pair.getFirst();
            Files.write(output.resolve("identity_asis.moc3"), moc);
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("bytes", moc.length);
            entry.put("sha256", sha256(output.resolve("identity_asis.moc3")));
            strategies.put("asis", entry);
            System.out.println("STRATEGY asis OK " + moc.length + " bytes");
        } catch (Throwable failure) {
            strategies.put("asis", Map.of("error", failure.getClass().getName() + ": " + String.valueOf(failure.getMessage())));
            System.out.println("STRATEGY asis FAILED " + failure);
        }

        // Strategy B ("canvas"): convert rest meshes to canvas space, then write.
        try {
            PuppetModel canvas = Moc3RestMeshKt.restMeshesToCanvasSpace(model, Map.of());
            Function2<DrawableId, float[], float[]> space = Moc3RestMeshKt.canvasToParentSpaceFor(canvas, Map.of());
            Pair<byte[], ?> pair = Moc3Export.INSTANCE.write(canvas, MocVersion.V50, space, exportOptions());
            byte[] moc = pair.getFirst();
            Files.write(output.resolve("identity_canvas.moc3"), moc);
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("bytes", moc.length);
            entry.put("sha256", sha256(output.resolve("identity_canvas.moc3")));
            strategies.put("canvas", entry);
            System.out.println("STRATEGY canvas OK " + moc.length + " bytes");
        } catch (Throwable failure) {
            strategies.put("canvas", Map.of("error", failure.getClass().getName() + ": " + String.valueOf(failure.getMessage())));
            System.out.println("STRATEGY canvas FAILED " + failure);
        }

        report.put("strategies", strategies);
        if (!sha256(archive).equals(inputSha) || !sha256(mainJar).equals(jarSha))
            throw new IllegalStateException("Input copy or JAR changed during authoring");
        Files.writeString(output.resolve("arm-author-report.json"), json(report) + "\n");
        System.out.println("ARM_AUTHOR_DONE");
    }

    public static void main(String[] args) {
        try {
            Map<String, Object> options = arguments(args);
            int seconds = Integer.parseInt((String) options.getOrDefault("--timeout-seconds", "120"));
            if (seconds < 1 || seconds > 600) throw new IllegalArgumentException("Timeout must be 1..600 seconds");
            Thread worker = new Thread(() -> {
                try { execute(options); }
                catch (Throwable failure) {
                    System.err.println("ARM_AUTHOR_FAILED " + failure.getClass().getName() + ": " + failure.getMessage());
                    failure.printStackTrace(System.err);
                    Runtime.getRuntime().halt(1);
                }
            }, "psd2live-isolated-arm-author");
            worker.setDaemon(true);
            worker.start();
            worker.join(seconds * 1000L);
            if (worker.isAlive()) { System.err.println("ARM_AUTHOR_TIMEOUT"); Runtime.getRuntime().halt(1); }
            Runtime.getRuntime().halt(0);
        } catch (Throwable failure) {
            System.err.println("ARM_AUTHOR_FAILED " + failure.getClass().getName() + ": " + failure.getMessage());
            failure.printStackTrace(System.err);
            Runtime.getRuntime().halt(1);
        }
    }
}
