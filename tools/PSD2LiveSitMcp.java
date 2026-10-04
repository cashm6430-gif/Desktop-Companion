import io.github.psd2live.agent.AgentMcpServiceKt;
import io.github.psd2live.agent.AgentProjectSnapshot;
import io.github.psd2live.agent.ViewModelAgentWorkspace;
import io.github.psd2live.project.ProjectArchive;
import io.github.psd2live.project.ProjectSession;
import io.github.psd2live.ui.state.PSD2LiveViewModel;
import io.ktor.server.cio.CIO;
import io.ktor.server.engine.EmbeddedServer;
import io.ktor.server.engine.EmbeddedServerKt;
import kotlin.ResultKt;
import kotlin.Unit;
import kotlin.coroutines.Continuation;
import kotlin.coroutines.CoroutineContext;
import kotlin.coroutines.EmptyCoroutineContext;
import kotlin.coroutines.intrinsics.IntrinsicsKt;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.time.Instant;
import java.util.HexFormat;
import java.util.List;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/**
 * Headless authoring MCP host for the sit/stand key-pose work. Opens a COPY of
 * the formal .psd2live archive in an isolated store (never the user's editing
 * workspace) and serves the same public authoring toolset the desktop app
 * exposes, on a localhost-only port with a session token written to a file.
 *
 * Like PSD2LiveRoundtrip, this calls only public API of the pinned 2.0.2 JAR:
 * ProjectSession.open, AgentMcpServiceKt.configureAgentMcp, Ktor embeddedServer.
 * No reflection, no private members, no MCP/private-method shortcuts. The
 * client drives mutations through MCP tools (deform/parameter/form/...); every
 * mutation is history-recoverable inside the isolated store and the source
 * archive bytes are re-verified at shutdown.
 *
 * Compile with a JDK 21:
 *   javac -encoding UTF-8 -cp "D:/tool/psd2live/app/*" -d <classes> tools/PSD2LiveSitMcp.java
 * Existing stores are never cleared or reused. The output files must not exist.
 * This host does not recover the old archive's missing runtime topology; use
 * the whole-model recovery checkpoint for future formal authoring.
 * Run with the same JDK 21:
 *   java -Dpsd2live.agent.store=<new-absolute-store>
 *     -Djava.io.tmpdir=<dedicated-writable-directory>
 *     -cp "<classes>;D:/tool/psd2live/app/*"
 *     PSD2LiveSitMcp --install D:/tool/psd2live --archive <absolute-copy.psd2live>
 *     --store <new-absolute-store> --port 23879
 *     --token-file <absolute-out>/mcp-token.txt --ready-file <absolute-out>/ready.txt
 *     --timeout-seconds 600
 */
public final class PSD2LiveSitMcp {
    public static final String EXPECTED_JAR_NAME =
        "psd2live-2.0.2-2ad13515cfa82896b5e2819475e01d.jar";
    public static final String EXPECTED_JAR_SHA256 =
        "f1e1663887a6ceefd57dd5cfa25f09b958fb45fbe5a0aa27a72e866bf7b5e3d4";

    private interface SuspendCall {
        Object invoke(Continuation<Object> continuation) throws Throwable;
    }

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
        if (initial == IntrinsicsKt.getCOROUTINE_SUSPENDED()) {
            result.get(Math.max(1, TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime())),
                TimeUnit.MILLISECONDS);
        } else {
            ResultKt.throwOnFailure(initial);
            result.complete(initial);
        }
        if (result.isCompletedExceptionally()) result.get();
        return result.getNow(null);
    }

    private static Path target(String value, String label) throws Exception {
        if (value == null || value.isBlank()) throw new IllegalArgumentException(label + " is required");
        Path path = Path.of(value);
        if (!path.isAbsolute()) throw new IllegalArgumentException(label + " must be absolute");
        path = path.normalize();
        Path ancestor = path;
        List<Path> suffix = new ArrayList<>();
        while (!Files.exists(ancestor, LinkOption.NOFOLLOW_LINKS)) {
            suffix.add(ancestor.getFileName());
            ancestor = ancestor.getParent();
            if (ancestor == null) throw new IllegalArgumentException(label + " has no real ancestor");
        }
        Path resolved = ancestor.toRealPath();
        if (!suffix.isEmpty() && !Files.isDirectory(resolved))
            throw new IllegalArgumentException(label + " ancestor is not a directory");
        for (int i = suffix.size() - 1; i >= 0; i--) resolved = resolved.resolve(suffix.get(i));
        return resolved.normalize();
    }

    private static boolean overlap(Path a, Path b) {
        return a.startsWith(b) || b.startsWith(a);
    }

    private static void fresh(Path path, String label) {
        if (Files.exists(path, LinkOption.NOFOLLOW_LINKS))
            throw new IllegalArgumentException(label + " must be fresh and nonexistent: " + path);
    }

    private static Map<String, String> options(String[] args) {
        List<String> required = List.of("--install", "--archive", "--store", "--token-file", "--ready-file");
        List<String> optional = List.of("--port", "--timeout-seconds", "--expected-archive-sha256");
        Map<String, String> result = new LinkedHashMap<>();
        if (args.length % 2 != 0) throw new IllegalArgumentException("Expected option/value pairs");
        for (int i = 0; i < args.length; i += 2) {
            String key = args[i];
            if ((!required.contains(key) && !optional.contains(key)) || result.containsKey(key))
                throw new IllegalArgumentException("Unknown or duplicate option: " + key);
            result.put(key, args[i + 1]);
        }
        for (String key : required)
            if (!result.containsKey(key)) throw new IllegalArgumentException("Missing " + key);
        return result;
    }

    private static String sha256(Path path) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (var stream = Files.newInputStream(path)) {
            byte[] block = new byte[1 << 20];
            int read;
            while ((read = stream.read(block)) > 0) digest.update(block, 0, read);
        }
        return HexFormat.of().formatHex(digest.digest());
    }

    public static void main(String[] args) throws Throwable {
        Map<String, String> options = options(args);
        Path install = target(options.get("--install"), "--install");
        Path archive = target(options.get("--archive"), "--archive");
        Path store = target(options.get("--store"), "--store");
        Path tokenFile = target(options.get("--token-file"), "--token-file");
        Path readyFile = target(options.get("--ready-file"), "--ready-file");
        int port = Integer.parseInt(options.getOrDefault("--port", "23879"));
        int seconds = Integer.parseInt(options.getOrDefault("--timeout-seconds", "900"));
        String bound = options.getOrDefault("--expected-archive-sha256", "");
        if (port < 1 || port > 65535 || seconds <= 0 || seconds > 86400)
            throw new IllegalArgumentException("Invalid port or timeout-seconds");
        if (!bound.isEmpty() && !bound.matches("[0-9a-f]{64}"))
            throw new IllegalArgumentException("Expected lowercase archive SHA256");
        if (!Files.isRegularFile(archive) || !archive.toString().toLowerCase().endsWith(".psd2live"))
            throw new IllegalArgumentException("Expected an existing .psd2live archive copy");
        String localData = System.getenv("LOCALAPPDATA");
        Path defaultStore = target((localData == null
            ? Path.of(System.getProperty("user.home"), ".psd2live", "agent-workspaces")
            : Path.of(localData, "PSD2Live", "agent-workspaces")).toAbsolutePath().toString(), "default store");
        List<Path> protectedPaths = List.of(archive, install, defaultStore);
        for (Path path : List.of(store, tokenFile, readyFile)) {
            fresh(path, "Isolated output");
            for (Path protectedPath : protectedPaths)
                if (overlap(path, protectedPath))
                    throw new IllegalArgumentException("Output overlaps source/install/default store: " + path);
        }
        if (overlap(store, tokenFile) || overlap(store, readyFile) || overlap(tokenFile, readyFile))
            throw new IllegalArgumentException("Store/token/ready paths must be disjoint");
        String property = System.getProperty("psd2live.agent.store");
        if (property == null || !target(property, "JVM store").equals(store))
            throw new IllegalArgumentException("Pass -Dpsd2live.agent.store equal to --store");
        // All path/JVM property checks precede any directory creation or app
        // construction. No pre-existing workspace is ever recursively cleared.
        String inputSha = sha256(archive);
        if (!bound.isEmpty() && !bound.equals(inputSha))
            throw new IllegalStateException("Archive copy hash differs from the pinned source: " + inputSha);

        Path mainJar = install.resolve("app").resolve(EXPECTED_JAR_NAME);
        String actualJarSha = sha256(mainJar);
        if (!EXPECTED_JAR_SHA256.equals(actualJarSha))
            throw new IllegalStateException("Unsupported PSD2Live JAR SHA256: " + actualJarSha);
        if (!Path.of(ProjectSession.class.getProtectionDomain().getCodeSource().getLocation().toURI())
                .toRealPath().equals(mainJar.toRealPath()))
            throw new IllegalStateException("Unexpected loaded PSD2Live JAR");
        System.setProperty("compose.application.resources.dir", install.resolve("app/resources").toString());
        System.setProperty("skiko.library.path", install.resolve("app").toString());
        System.setProperty("jpackage.app-version", "2.0.2");

        PSD2LiveViewModel viewModel = null;
        ViewModelAgentWorkspace workspace = null;
        EmbeddedServer<?, ?> server = null;
        String readyPayload = null;
        boolean readyCreated = false;
        try {
            Files.createDirectories(store.getParent());
            Files.createDirectory(store);
            Files.createDirectories(tokenFile.getParent());
            Files.createDirectories(readyFile.getParent());
            viewModel = new PSD2LiveViewModel();
            workspace = new ViewModelAgentWorkspace(viewModel, store);
            viewModel.attachAgentWorkspace(workspace);
            final ViewModelAgentWorkspace active = workspace;
            ProjectSession project = new ProjectSession(viewModel, (directory, target, id) -> {
                ProjectArchive.INSTANCE.write(directory, target, id);
                return Unit.INSTANCE;
            });
            System.out.println("OPEN " + archive);
            final ProjectSession openSession = project;
            await(cont -> {
                try {
                    return openSession.open(active, archive, cont);
                } catch (Throwable failure) {
                    if (failure instanceof RuntimeException runtime) throw runtime;
                    if (failure instanceof Error error) throw error;
                    throw new RuntimeException(failure);
                }
            }, System.nanoTime() + TimeUnit.SECONDS.toNanos(120));
            AgentProjectSnapshot snapshot = active.snapshot();
            String head = snapshot.getHistoryHeadNodeId();
            if (head == null) throw new IllegalStateException("Opened archive has no authored HEAD");
            System.out.println("PROJECT id=" + snapshot.getProjectId() + " head=" + head
                + " parameters=" + snapshot.getParameters().size());

            byte[] raw = new byte[24];
            new SecureRandom().nextBytes(raw);
            String token = HexFormat.of().formatHex(raw);
            final String bearer = token;
            server = EmbeddedServerKt.embeddedServer(CIO.INSTANCE, port, "127.0.0.1", List.of(),
                (app, cont) -> {
                    AgentMcpServiceKt.configureAgentMcp(app, active, bearer,
                        AgentMcpServiceKt.DEFAULT_MCP_MAX_REQUEST_BODY_BYTES);
                    return Unit.INSTANCE;
                });
            server.start(false);
            Files.writeString(tokenFile, token + "\n", StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
            readyPayload = Instant.now() + " port=" + port + " head=" + head + "\n";
            Files.writeString(readyFile, readyPayload, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
            readyCreated = true;
            System.out.println("READY port=" + port + " token-file=" + tokenFile);

            long idleDeadline = System.currentTimeMillis() + seconds * 1000L;
            Path stopFile = readyFile.resolveSibling("sit-mcp.stop");
            while (System.currentTimeMillis() < idleDeadline) {
                if (Files.exists(stopFile)) {
                    System.out.println("STOP file seen");
                    break;
                }
                Thread.sleep(500);
            }
        } finally {
            if (readyCreated && Files.isRegularFile(readyFile, LinkOption.NOFOLLOW_LINKS)
                    && readyPayload.equals(Files.readString(readyFile))) Files.delete(readyFile);
            if (server != null) {
                try { server.stop(500, 2000, TimeUnit.MILLISECONDS); } catch (Throwable ignored) { }
            }
            if (workspace != null) {
                try { workspace.close(); } catch (Throwable ignored) { }
            }
            if (viewModel != null) {
                try { viewModel.close(); } catch (Throwable ignored) { }
            }
            if (!inputSha.equals(sha256(archive)) || !actualJarSha.equals(sha256(mainJar)))
                throw new IllegalStateException("Source archive or pinned JAR changed during authoring");
        }
        System.out.println("DONE");
    }
}
