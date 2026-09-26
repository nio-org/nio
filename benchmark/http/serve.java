// The Java side of the HTTP server benchmark. Same three routes as serve.nio,
// on the JDK's own server (com.sun.net.httpserver) with one virtual thread per
// request, and no library from outside the JDK. The JVM uses every core, so the
// runner times it again with -XX:ActiveProcessorCount=1.
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.function.Function;

class Serve {
    record Answer(int status, String type, String body) {}

    record Route(String method, String[] segments, Function<Map<String, String>, Answer> handler) {}

    static final List<Route> routes = new ArrayList<>();

    static void route(String method, String pattern, Function<Map<String, String>, Answer> h) {
        routes.add(new Route(method, split(pattern), h));
    }

    static String[] split(String path) {
        return java.util.Arrays.stream(path.split("/")).filter(s -> !s.isEmpty()).toArray(String[]::new);
    }

    // The linear route scan the Nio and Node.js servers use.
    static Map<String, String> match(Route r, String method, String[] got) {
        if (!r.method().equals(method) || r.segments().length != got.length) return null;
        Map<String, String> params = new HashMap<>();
        for (int i = 0; i < got.length; i++) {
            String want = r.segments()[i];
            if (want.startsWith(":")) {
                params.put(want.substring(1), got[i]);
            } else if (!want.equals(got[i])) {
                return null;
            }
        }
        return params;
    }

    static void handle(HttpExchange ex) throws IOException {
        String path = ex.getRequestURI().getPath();
        String[] got = split(path);
        Answer out = new Answer(404, "text/plain; charset=utf-8", "not found");
        for (Route r : routes) {
            Map<String, String> params = match(r, ex.getRequestMethod(), got);
            if (params == null) continue;
            out = r.handler().apply(params);
            break;
        }
        byte[] body = out.body().getBytes(StandardCharsets.UTF_8);
        ex.getResponseHeaders().set("Content-Type", out.type());
        ex.sendResponseHeaders(out.status(), body.length);
        try (OutputStream os = ex.getResponseBody()) {
            os.write(body);
        }
    }

    public static void main(String[] args) throws IOException {
        route("GET", "/plaintext", p -> new Answer(200, "text/plain; charset=utf-8", "Hello, World!"));
        // The JDK has no JSON library. The object is a constant, so the text is
        // what a serializer writes for it.
        route("GET", "/json", p -> new Answer(200, "application/json", "{\"message\":\"Hello, World!\"}"));
        route("GET", "/users/:id", p -> new Answer(200, "text/plain; charset=utf-8", "user " + p.get("id")));

        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 128);
        server.createContext("/", Serve::handle);
        server.setExecutor(Executors.newVirtualThreadPerTaskExecutor());
        server.start();
        System.out.println(server.getAddress().getPort());
        System.out.flush();
    }
}
