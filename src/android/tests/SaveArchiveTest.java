import org.nxemu.features.saves.SaveArchive;
import java.io.*;
import java.nio.file.*;
import java.util.zip.*;

public class SaveArchiveTest {
    static final String TITLE = "0100123456789000";
    static byte[] zip(String... paths) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (String path : paths) {
                zip.putNextEntry(new ZipEntry(path));
                if (!path.endsWith("/")) zip.write("new-data".getBytes("UTF-8"));
                zip.closeEntry();
            }
        }
        return bytes.toByteArray();
    }
    static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    static void rejected(Path root, String... names) throws Exception {
        File stage = Files.createTempDirectory(root, "reject-").toFile();
        try {
            SaveArchive.unpack(new ByteArrayInputStream(zip(names)), stage);
            throw new AssertionError("Accepted invalid ZIP");
        } catch (IOException expected) { } finally { SaveArchive.deleteTree(stage); }
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("nxemu-saves-");
        try {
            rejected(root, "../escape");
            rejected(root, TITLE + "/../escape");
            rejected(root, "/" + TITLE + "/data");
            rejected(root, TITLE + "\\data");
            rejected(root, "raw-save-file");
            rejected(root, TITLE + "/");
            rejected(root, TITLE + "/data", "0100000000000001/data");
            rejected(root, TITLE + "/data", TITLE + "/DATA");
            File stage = root.resolve("staged").toFile();
            SaveArchive.Bundle bundle = SaveArchive.unpack(new ByteArrayInputStream(zip(TITLE + "/sub/data")), stage);
            File live = root.resolve("live").toFile();
            Files.createDirectories(live.toPath());
            Files.write(live.toPath().resolve("old"), "original".getBytes("UTF-8"));
            File badBackupDir = root.resolve("not-a-directory").toFile();
            Files.write(badBackupDir.toPath(), new byte[]{1});
            try {
                SaveArchive.replace(live, bundle, badBackupDir);
                throw new AssertionError("Backup failure must prevent replacement");
            } catch (IOException expected) { }
            check(new String(Files.readAllBytes(live.toPath().resolve("old")), "UTF-8").equals("original"), "Original save was modified on failure");
            File backup = SaveArchive.replace(live, bundle, root.resolve("backups").toFile());
            check(backup != null && backup.isFile(), "No backup retained");
            check(!new File(live, "old").exists() && new File(live, "sub/data").isFile(), "Replacement merged stale files");
            SaveArchive.Bundle restore;
            try (InputStream in = new FileInputStream(backup)) {
                restore = SaveArchive.unpack(in, root.resolve("restore").toFile());
            }
            check(new String(Files.readAllBytes(new File(restore.payload, "old").toPath()), "UTF-8").equals("original"), "Backup did not preserve original bytes");
            SaveArchive.replace(live, restore, root.resolve("backups").toFile());
            check(new File(live, "old").isFile() && !new File(live, "sub/data").exists(), "Restore failed");
            ByteArrayOutputStream exported = new ByteArrayOutputStream();
            SaveArchive.exportZip(live, TITLE, "device", exported);
            SaveArchive.Bundle roundTrip = SaveArchive.unpack(new ByteArrayInputStream(exported.toByteArray()), root.resolve("roundtrip").toFile());
            check(roundTrip.kind.equals("device"), "Device saves restored to the wrong kind");
            check(new File(roundTrip.payload, "old").isFile(), "Round trip lost files");
            System.out.println("PASS: Eden ZIP, traversal/duplicate/multi-title/empty rejection, backup failure preserves original, replacement, backup restore, device round trip");
        } finally { SaveArchive.deleteTree(root.toFile()); }
    }
}
