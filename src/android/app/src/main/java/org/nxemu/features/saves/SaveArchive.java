package org.nxemu.features.saves;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.*;
import java.util.zip.*;

/** Eden-compatible single-title ZIPs. No writes to live saves until staging and backup finish. */
public final class SaveArchive {
    private static final long MAX_BYTES = 4L * 1024 * 1024 * 1024;
    private static final int MAX_ENTRIES = 100000;
    private static final String META = "nxemu-save.properties";

    public static final class Bundle {
        public final String titleId;
        public final String kind;
        public final File payload;
        Bundle(String titleId, String kind, File payload) {
            this.titleId = titleId; this.kind = kind; this.payload = payload;
        }
    }

    public static Bundle unpack(InputStream source, File staging) throws IOException {
        Files.createDirectories(staging.toPath());
        Set<String> names = new HashSet<>();
        String title = null;
        long total = 0;
        int count = 0, files = 0;
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(source))) {
            ZipEntry entry;
            byte[] buffer = new byte[65536];
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName();
                if (++count > MAX_ENTRIES || name.isEmpty() || name.startsWith("/") ||
                    name.contains("\\") || name.indexOf('\0') >= 0) throw new IOException("无效的 ZIP 路径");
                String normalized = name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
                String[] parts = normalized.split("/", -1);
                for (String part : parts) if (part.isEmpty() || part.equals(".") || part.equals(".."))
                    throw new IOException("ZIP 含有越界路径");
                if (!names.add(normalized.toLowerCase(Locale.ROOT))) throw new IOException("ZIP 含重复文件");
                if (!normalized.equals(META)) {
                    if (!parts[0].matches("(?i)[0-9a-f]{16}"))
                        throw new IOException("需要 Eden 格式：ZIP 最外层为 16 位 Title ID 文件夹");
                    if (title != null && !title.equals(parts[0])) throw new IOException("请选择单个游戏的存档 ZIP");
                    title = parts[0];
                    if (parts.length == 1 && !entry.isDirectory()) throw new IOException("Title ID 必须是文件夹");
                } else if (entry.isDirectory()) throw new IOException("无效的存档信息");
                Path target = staging.toPath().resolve(normalized).normalize();
                if (!target.startsWith(staging.toPath())) throw new IOException("ZIP 路径越界");
                if (entry.isDirectory()) {
                    Files.createDirectories(target);
                } else {
                    Files.createDirectories(target.getParent());
                    long size = 0;
                    try (OutputStream out = Files.newOutputStream(target, StandardOpenOption.CREATE_NEW)) {
                        int n;
                        while ((n = zip.read(buffer)) != -1) {
                            total += n; size += n;
                            if (total > MAX_BYTES || (normalized.equals(META) && size > 4096))
                                throw new IOException("存档 ZIP 超出大小限制");
                            out.write(buffer, 0, n);
                        }
                    }
                    if (!normalized.equals(META)) files++;
                }
                zip.closeEntry();
            }
        }
        if (title == null || files == 0) throw new IOException("ZIP 中没有游戏存档文件");
        String kind = "account";
        File meta = new File(staging, META);
        if (meta.isFile()) {
            Properties properties = new Properties();
            try (InputStream in = new FileInputStream(meta)) { properties.load(in); }
            String declared = properties.getProperty("titleId", title);
            if (!declared.equalsIgnoreCase(title)) throw new IOException("存档 Title ID 不匹配");
            kind = properties.getProperty("kind", "account");
            if (!kind.equals("account") && !kind.equals("device")) throw new IOException("不支持的存档类型");
        }
        return new Bundle(title.toUpperCase(Locale.ROOT), kind, new File(staging, title));
    }

    public static void exportZip(File directory, String titleId, String kind, OutputStream output) throws IOException {
        if (!titleId.matches("(?i)[0-9a-f]{16}") || !directory.isDirectory()) throw new IOException("没有找到存档");
        Path root = directory.toPath();
        try (ZipOutputStream zip = new ZipOutputStream(new BufferedOutputStream(output))) {
            zip.putNextEntry(new ZipEntry(META));
            zip.write(("titleId=" + titleId + "\nkind=" + kind + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
            zip.closeEntry();
            Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes attrs) throws IOException {
                    if (attrs.isSymbolicLink()) throw new IOException("不支持符号链接");
                    String relative = root.relativize(dir).toString().replace(File.separatorChar, '/');
                    zip.putNextEntry(new ZipEntry(titleId + "/" + (relative.isEmpty() ? "" : relative + "/")));
                    zip.closeEntry();
                    return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                    if (!attrs.isRegularFile() || Files.isSymbolicLink(file)) throw new IOException("不支持特殊存档文件");
                    zip.putNextEntry(new ZipEntry(titleId + "/" + root.relativize(file).toString().replace(File.separatorChar, '/')));
                    Files.copy(file, zip);
                    zip.closeEntry();
                    return FileVisitResult.CONTINUE;
                }
            });
        }
    }

    /** Returns the retained backup ZIP, or null for a new save. */
    public static File replace(File destination, Bundle bundle, File backups) throws IOException {
        Path parent = destination.toPath().getParent();
        Files.createDirectories(parent);
        File fresh = new File(parent.toFile(), ".nxemu-new-" + UUID.randomUUID());
        File retired = new File(parent.toFile(), ".nxemu-old-" + UUID.randomUUID());
        File backup = null;
        try {
            Files.walkFileTree(bundle.payload.toPath(), new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult preVisitDirectory(Path dir, BasicFileAttributes attrs) throws IOException {
                    Files.createDirectories(fresh.toPath().resolve(bundle.payload.toPath().relativize(dir)));
                    return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                    if (!attrs.isRegularFile() || Files.isSymbolicLink(file)) throw new IOException("不支持特殊文件");
                    Files.copy(file, fresh.toPath().resolve(bundle.payload.toPath().relativize(file)));
                    return FileVisitResult.CONTINUE;
                }
            });
            if (destination.exists()) {
                Files.createDirectories(backups.toPath());
                backup = new File(backups, bundle.titleId + "-" + bundle.kind + "-" + UUID.randomUUID() + ".zip");
                try (OutputStream out = new FileOutputStream(backup)) {
                    exportZip(destination, bundle.titleId, bundle.kind, out);
                }
                // Read the backup to completion before replacing the live directory (CRC validation).
                try (ZipInputStream zip = new ZipInputStream(new FileInputStream(backup))) {
                    byte[] data = new byte[65536];
                    while (zip.getNextEntry() != null) {
                        while (zip.read(data) != -1) { /* validates CRC at entry end */ }
                        zip.closeEntry();
                    }
                }
                if (!destination.renameTo(retired)) throw new IOException("无法保留原存档，未执行替换");
            }
            if (!fresh.renameTo(destination)) {
                if (retired.exists() && !retired.renameTo(destination))
                    throw new IOException("替换失败，原存档保留在：" + retired);
                throw new IOException("存档替换失败，原存档已恢复");
            }
            try { deleteTree(retired); } catch (IOException ignored) { /* old save remains as an additional backup */ }
            return backup;
        } finally {
            deleteTree(fresh);
        }
    }

    public static void deleteTree(File directory) throws IOException {
        if (!Files.exists(directory.toPath(), LinkOption.NOFOLLOW_LINKS)) return;
        Files.walkFileTree(directory.toPath(), new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                Files.delete(file); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path dir, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(dir); return FileVisitResult.CONTINUE;
            }
        });
    }
    private SaveArchive() {}
}
