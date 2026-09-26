// The Java server for the visual HTTP benchmark. Same routes as server.nio,
// through the JDK's com.sun.net.httpserver on a virtual-thread executor
// (Java 21+). The JDK has no JSON parser, so a minimal one sits below.
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

public class Server {

    public static void main(String[] args) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 1024);
        server.createContext("/", Server::route);
        server.setExecutor(Executors.newVirtualThreadPerTaskExecutor());
        server.start();
        System.out.println(server.getAddress().getPort());
        System.out.flush();
    }

    static void route(HttpExchange ex) throws IOException {
        String path = ex.getRequestURI().getPath();
        String method = ex.getRequestMethod();
        if (method.equals("GET") && path.equals("/plaintext")) {
            reply(ex, 200, "text/plain; charset=utf-8", "Hello, World!");
        } else if (method.equals("GET") && path.equals("/json")) {
            reply(ex, 200, "application/json", "{\"message\":\"Hello, World!\"}");
        } else if (method.equals("GET") && path.startsWith("/users/")) {
            String id = path.substring("/users/".length());
            if (id.isEmpty()) reply(ex, 400, "text/plain; charset=utf-8", "missing id");
            else reply(ex, 200, "text/plain; charset=utf-8", "user " + id);
        } else if (method.equals("POST") && path.equals("/data")) {
            data(ex);
        } else {
            reply(ex, 404, "text/plain; charset=utf-8", "not found");
        }
    }

    static void data(HttpExchange ex) throws IOException {
        byte[] body = ex.getRequestBody().readAllBytes();
        Object root;
        try {
            root = new Json(new String(body, StandardCharsets.UTF_8)).parse();
        } catch (RuntimeException e) {
            reply(ex, 400, "text/plain; charset=utf-8", "bad json: " + e.getMessage());
            return;
        }
        @SuppressWarnings("unchecked")
        List<Object> items = (List<Object>) ((Map<?, ?>) root).get("items");
        double total = 0;
        for (Object o : items) {
            total += (Double) ((Map<?, ?>) o).get("price");
        }
        reply(ex, 200, "application/json",
                "{\"count\":" + items.size() + ",\"total\":" + total + "}");
    }

    static void reply(HttpExchange ex, int status, String type, String body) throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        ex.getResponseHeaders().set("Content-Type", type);
        ex.sendResponseHeaders(status, bytes.length);
        try (OutputStream os = ex.getResponseBody()) {
            os.write(bytes);
        }
    }

    /** Minimal JSON: objects to HashMap, arrays to ArrayList, numbers to
     *  Double, plus String, Boolean and null. */
    static final class Json {
        private final String s;
        private int i;

        Json(String s) { this.s = s; }

        Object parse() {
            Object v = value();
            ws();
            if (i != s.length()) throw err("trailing data");
            return v;
        }

        private Object value() {
            ws();
            char c = peek();
            switch (c) {
                case '{': return object();
                case '[': return array();
                case '"': return string();
                case 't': expect("true"); return Boolean.TRUE;
                case 'f': expect("false"); return Boolean.FALSE;
                case 'n': expect("null"); return null;
                default: return number();
            }
        }

        private Map<String, Object> object() {
            Map<String, Object> m = new HashMap<>();
            i++; // {
            ws();
            if (peek() == '}') { i++; return m; }
            while (true) {
                ws();
                String k = string();
                ws();
                if (s.charAt(i++) != ':') throw err("expected ':'");
                m.put(k, value());
                ws();
                char c = s.charAt(i++);
                if (c == '}') return m;
                if (c != ',') throw err("expected ',' or '}'");
            }
        }

        private List<Object> array() {
            List<Object> a = new ArrayList<>();
            i++; // [
            ws();
            if (peek() == ']') { i++; return a; }
            while (true) {
                a.add(value());
                ws();
                char c = s.charAt(i++);
                if (c == ']') return a;
                if (c != ',') throw err("expected ',' or ']'");
            }
        }

        private String string() {
            if (s.charAt(i++) != '"') throw err("expected string");
            StringBuilder b = new StringBuilder();
            while (true) {
                char c = s.charAt(i++);
                if (c == '"') return b.toString();
                if (c != '\\') { b.append(c); continue; }
                char e = s.charAt(i++);
                switch (e) {
                    case '"': b.append('"'); break;
                    case '\\': b.append('\\'); break;
                    case '/': b.append('/'); break;
                    case 'b': b.append('\b'); break;
                    case 'f': b.append('\f'); break;
                    case 'n': b.append('\n'); break;
                    case 'r': b.append('\r'); break;
                    case 't': b.append('\t'); break;
                    case 'u':
                        b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                        i += 4;
                        break;
                    default: throw err("bad escape");
                }
            }
        }

        private Double number() {
            int start = i;
            if (peek() == '-') i++;
            while (i < s.length() && "0123456789.eE+-".indexOf(s.charAt(i)) >= 0) i++;
            if (i == start) throw err("expected value");
            return Double.parseDouble(s.substring(start, i));
        }

        private void expect(String word) {
            if (!s.startsWith(word, i)) throw err("expected " + word);
            i += word.length();
        }

        private void ws() {
            while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++;
        }

        private char peek() {
            if (i >= s.length()) throw err("unexpected end");
            return s.charAt(i);
        }

        private RuntimeException err(String msg) {
            return new RuntimeException(msg + " at " + i);
        }
    }
}
