package io.github.ttsnap.clingo;

import org.springframework.stereotype.Controller;
import org.springframework.ui.Model;
import org.springframework.web.bind.annotation.*;

@Controller
public class ClingoController {

    private static final String DEFAULT_PROGRAM = """
            % Graph coloring example (3 colors, 4 nodes)
            node(1..4).
            color(red;green;blue).
            edge(1,2). edge(1,3). edge(2,3). edge(2,4). edge(3,4).

            % Assign exactly one color per node
            { assign(N, C) : color(C) } = 1 :- node(N).

            % Adjacent nodes must have different colors
            :- edge(X, Y), assign(X, C), assign(Y, C).

            #show assign/2.
            """;

    @GetMapping("/")
    public String index(Model model) {
        model.addAttribute("program", DEFAULT_PROGRAM);
        return "index";
    }

    @PostMapping("/run")
    public String run(@RequestParam String program, Model model) {
        model.addAttribute("program", program);
        model.addAttribute("result", ClingoRunner.run(program));
        return "index";
    }
}
