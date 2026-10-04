import io.github.psd2live.agent.AgentProjectSnapshot;
import io.github.psd2live.agent.ViewModelAgentWorkspace;
import io.github.psd2live.project.ProjectArchive;
import io.github.psd2live.project.ProjectSession;
import io.github.psd2live.ui.state.PSD2LiveViewModel;
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
 * Independent, version-pinned PSD2Live project reader/exporter. This never calls
 * MainKt/runGui, MCP, private methods, reflection, or another application's JVM.
 * It uses the exact public ProjectSession/Workspace API in the installed 2.0.2
 * JAR. That application API is not a promised stable library interface: an
 * upgrade needs a new signature audit, hash pin, and isolated fixture smoke test.
 *
 * Compile with a JDK 21 (the app's trimmed runtime has no javac):
 *   javac -encoding UTF-8 -cp "D:/tool/psd2live/app/*" -d <classes> tools/PSD2LiveRoundtrip.java
 * Run with the same JDK 21 and the installation's application dependencies:
 *   java -Dpsd2live.agent.store=<new-absolute-store>
 *     -Djava.io.tmpdir=<dedicated-writable-build-directory>
 *     -cp "<classes>;D:/tool/psd2live/app/*"
 *     PSD2LiveRoundtrip --install D:/tool/psd2live --archive <absolute-copy.psd2live>
 *     --output <new-absolute-output> --store <new-absolute-store> --timeout-seconds 120
 *
 * --store must agree with the JVM property, and both store/output must be new or
 * empty, disjoint paths outside the default store. Input bytes are checked before
 * and after export. ProjectSession extracts the input to its own temporary store;
 * opening a copied archive therefore does not change the source workspace HEAD.
 * Do not supply a production archive until the version-matched small fixture has
 * passed. A successful export says nothing about visual approval or MOC fidelity.
 * The JSON report records only this library execution and output file hashes.
 */
public final class PSD2LiveRoundtrip {
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
        List<String> allowed = List.of("--install", "--archive", "--output", "--store", "--timeout-seconds");
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

    private static void execute(Map<String, String> options, long deadline) throws Throwable {
        Path install = absolute(options.get("--install"), "--install").toRealPath();
        Path archive = absolute(options.get("--archive"), "--archive").toRealPath();
        Path output = target(options.get("--output"), "--output");
        Path store = target(options.get("--store"), "--store");
        if (!Files.isRegularFile(archive) || !archive.toString().toLowerCase().endsWith(".psd2live"))
            throw new IllegalArgumentException("Expected an existing .psd2live archive copy");
        if (overlap(output, store) || overlap(output, install) || overlap(store, install)
                || archive.startsWith(output) || archive.startsWith(store))
            throw new IllegalArgumentException("Output/store must be disjoint from installation and input");
        String defaultRoot = System.getenv("LOCALAPPDATA");
        Path originalStore = defaultRoot == null ? Path.of(System.getProperty("user.home"), ".psd2live", "agent-workspaces")
            : Path.of(defaultRoot, "PSD2Live", "agent-workspaces");
        Path resolvedOriginalStore = target(originalStore.toAbsolutePath().toString(), "default store");
        if (overlap(store, resolvedOriginalStore) || overlap(output, resolvedOriginalStore))
            throw new IllegalArgumentException("Refusing the default authoring store or its parent");
        String property = System.getProperty("psd2live.agent.store");
        if (property == null || !store.equals(target(property, "psd2live.agent.store")))
            throw new IllegalArgumentException("Pass -Dpsd2live.agent.store equal to --store before JVM startup");
        emptyDirectory(output);
        emptyDirectory(store);
        Path mainJar = install.resolve("app").resolve(EXPECTED_JAR_NAME);
        String actualJarSha = sha256(mainJar);
        if (!EXPECTED_JAR_SHA256.equals(actualJarSha))
            throw new IllegalStateException("Unsupported PSD2Live JAR SHA256; audit/recompile for this version: " + actualJarSha);
        String inputSha = sha256(archive);
        System.setProperty("compose.application.resources.dir", install.resolve("app/resources").toString());
        System.setProperty("skiko.library.path", install.resolve("app").toString());
        System.setProperty("jpackage.app-version", "2.0.2");
        URI loadedFrom = PSD2LiveViewModel.class.getProtectionDomain().getCodeSource().getLocation().toURI();
        if (!Path.of(loadedFrom).toRealPath().equals(mainJar.toRealPath()))
            throw new IllegalStateException("Classpath loaded a different PSD2Live JAR: " + loadedFrom);
        Files.createDirectories(store);
        Files.createDirectories(output);
        System.out.println("VERSION_BOUND 2.0.2 " + actualJarSha);
        System.out.println("OPEN " + archive);
        PSD2LiveViewModel viewModel = null;
        ViewModelAgentWorkspace workspace = null;
        Throwable failure = null;
        try {
            viewModel = new PSD2LiveViewModel();
            workspace = new ViewModelAgentWorkspace(viewModel, store);
            viewModel.attachAgentWorkspace(workspace);
            ProjectSession project = new ProjectSession(viewModel, (directory, target, id) -> {
                ProjectArchive.INSTANCE.write(directory, target, id);
                return Unit.INSTANCE;
            });
            final ViewModelAgentWorkspace active = workspace;
            await(cont -> project.open(active, archive, cont), deadline);
            AgentProjectSnapshot before = active.snapshot();
            String head = before.getHistoryHeadNodeId();
            if (head == null) throw new IllegalStateException("Opened archive has no authored HEAD");
            System.out.println("EXPORT project=" + before.getProjectId() + " head=" + head);
            Object exported = await(cont -> active.exportModel(head, output.toString(), cont), deadline);
            if (!inputSha.equals(sha256(archive)) || !actualJarSha.equals(sha256(mainJar)))
                throw new IllegalStateException("Input archive or version-bound JAR changed during export");
            AgentProjectSnapshot after = active.snapshot();
            if (!head.equals(after.getHistoryHeadNodeId()))
                throw new IllegalStateException("Authoring HEAD changed during export");
            Map<String, String> outputHashes = new LinkedHashMap<>();
            try (var paths = Files.walk(output)) {
                for (Path path : paths.filter(Files::isRegularFile).sorted().toList()) {
                    outputHashes.put(output.relativize(path).toString().replace('\\', '/'), sha256(path));
                }
            }
            if (outputHashes.keySet().stream().noneMatch(name -> name.endsWith(".moc3")))
                throw new IllegalStateException("Library export returned without a MOC3 output");
            Map<String, Object> report = new LinkedHashMap<>();
            report.put("schema_version", 1);
            report.put("kind", "version_bound_public_api_library_roundtrip");
            report.put("created_utc", Instant.now().toString());
            report.put("status", "exported_visual_fidelity_unreviewed");
            report.put("main_jar", mainJar.toString());
            report.put("main_jar_sha256", actualJarSha);
            report.put("archive", archive.toString());
            report.put("archive_sha256_before_and_after", inputSha);
            report.put("isolated_store", store.toString());
            report.put("project_id", before.getProjectId());
            report.put("history_head_before_and_after", head);
            report.put("parameter_count", before.getParameters().size());
            report.put("export_result_type", exported == null ? "null" : exported.getClass().getName());
            report.put("output_hashes", outputHashes);
            Files.writeString(output.resolve("library-roundtrip.json"), json(report) + "\n");
            System.out.println("EXPORTED " + outputHashes.size() + " files; visual fidelity remains unreviewed");
        } catch (Throwable caught) {
            failure = caught;
            throw caught;
        } finally {
            Throwable closeFailure = null;
            try {
                if (workspace != null) workspace.close();
            } catch (Throwable caught) {
                closeFailure = caught;
            }
            try {
                if (viewModel != null) viewModel.close();
            } catch (Throwable caught) {
                if (closeFailure == null) closeFailure = caught;
                else closeFailure.addSuppressed(caught);
            }
            if (closeFailure != null) {
                if (failure != null) failure.addSuppressed(closeFailure);
                else throw closeFailure;
            }
        }
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
