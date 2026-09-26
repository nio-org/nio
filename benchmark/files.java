// The Java version of files.nio. Java strings are immutable, as in Nio.
// java.util.regex is a backtracking engine.
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

class Main {
    public static void main(String[] args) throws IOException {
        long n = 1000;
        long lines = 400;

        String doc = "";
        long k = 0;
        while (k < lines) {
            doc = doc + "field_" + k + " = value_" + k + "\n";
            k = k + 1;
        }
        doc = doc + "counter = 0000\n";

        Path dir = Files.createTempDirectory("nio-bench-files-");
        Path file = dir.resolve("data.txt");
        Files.write(file, doc.getBytes(StandardCharsets.UTF_8));

        Pattern re = Pattern.compile("counter = [0-9]{4}");
        long checksum = 0;
        long i = 0;

        while (i < n) {
            String text = new String(Files.readAllBytes(file), StandardCharsets.UTF_8);
            Matcher m = re.matcher(text);
            m.find();
            int at = m.start();
            checksum = checksum + at;

            long v = Long.parseLong(text.substring(at + 10, at + 14));
            v = v + 1;
            String num = String.format("%04d", v);
            String edited = text.substring(0, at + 10) + num + text.substring(at + 14);

            Files.write(file, edited.getBytes(StandardCharsets.UTF_8));
            i = i + 1;
        }

        String text = new String(Files.readAllBytes(file), StandardCharsets.UTF_8);
        Matcher m = re.matcher(text);
        m.find();
        int at = m.start();
        long counter = Long.parseLong(text.substring(at + 10, at + 14));

        Files.delete(file);
        Files.delete(dir);
        System.out.println(checksum + counter);
    }
}
