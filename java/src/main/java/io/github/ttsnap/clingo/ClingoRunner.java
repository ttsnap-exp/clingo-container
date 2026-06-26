package io.github.ttsnap.clingo;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.concurrent.TimeUnit;

public class ClingoRunner {

    private static final int TIMEOUT_SECONDS = 30;

    public static String run(String program) {
        Path tmp = null;
        try {
            tmp = Files.createTempFile("clingo_", ".lp");
            Files.writeString(tmp, program, StandardCharsets.UTF_8);

            ProcessBuilder pb = new ProcessBuilder("clingo", tmp.toString(), "0");
            pb.redirectErrorStream(true);
            Process process = pb.start();

            boolean finished = process.waitFor(TIMEOUT_SECONDS, TimeUnit.SECONDS);
            if (!finished) {
                process.destroyForcibly();
                return "ERROR: Clingo timed out after " + TIMEOUT_SECONDS + " seconds.";
            }

            return new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);

        } catch (IOException e) {
            return "ERROR: Could not execute clingo — is it installed?\n" + e.getMessage();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "ERROR: Execution interrupted.";
        } finally {
            if (tmp != null) {
                try { Files.deleteIfExists(tmp); } catch (IOException ignored) {}
            }
        }
    }
}
