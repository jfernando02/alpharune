/// @file nn_eval.cpp
/// Runs a neural model (agents/nn_model) over recorded positions.
///
///   riftbound_nn_eval --model FILE --data PREFIX [--limit N] [--out FILE] [--bench N]
///
/// For each of the first N positions of the shard PREFIX (ml/obs_io.h) it
/// writes one line to --out (default stdout):
///
///   <position> <value logit> <policy logit of action 0> <action 1> ...
///
/// riftbound-trainer's nn_export.py compares them with PyTorch's outputs for
/// the same positions: the engine's forward pass is written by hand, and this
/// is the check that it computes what was trained.
///
/// --bench N repeats the forward pass N times over those positions and
/// reports the mean time of one policy + value evaluation.

#include "agents/nn_model.h"
#include "ml/obs_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace riftbound;

int main(int argc, char** argv) {
    std::string model_path, data, out_path;
    long limit = 200, bench = 0;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--model") model_path = next();
            else if (a == "--data") data = next();
            else if (a == "--limit") limit = std::stol(next());
            else if (a == "--out") out_path = next();
            else if (a == "--bench") bench = std::stol(next());
            else throw std::runtime_error("unknown argument " + a);
        }
        if (model_path.empty() || data.empty()) throw std::runtime_error("--model and --data are required");
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n"
                  << "usage: riftbound_nn_eval --model FILE --data PREFIX [--limit N] [--out FILE] [--bench N]\n";
        return 2;
    }

    try {
        const auto model = NnModel::load(model_path);
        auto positions = ml::readShard(data, static_cast<size_t>(std::max(0L, limit)));

        std::FILE* out = out_path.empty() ? stdout : std::fopen(out_path.c_str(), "w");
        if (!out) throw std::runtime_error("cannot write " + out_path);
        std::vector<double> logits;
        for (size_t p = 0; p < positions.size(); ++p) {
            double value = 0.0;
            // Both heads, whatever the file says it covers: the weights are
            // always exported, and this is a check of the arithmetic.
            model->forward(positions[p].obs, &positions[p].actions, &value, &logits);
            std::fprintf(out, "%zu %.6f", p, value);
            for (double l : logits) std::fprintf(out, " %.6f", l);
            std::fprintf(out, "\n");
        }
        if (out != stdout) std::fclose(out);

        if (bench > 0 && !positions.empty()) {
            long evals = 0, actions = 0;
            double sink = 0.0;
            const auto start = std::chrono::steady_clock::now();
            for (long b = 0; b < bench; ++b)
                for (ml::ObsSample& p : positions) {
                    double value = 0.0;
                    model->forward(p.obs, &p.actions, &value, &logits);
                    sink += value;
                    ++evals;
                    actions += static_cast<long>(p.actions.size());
                }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cerr << evals << " evaluations, " << 1e6 * secs / evals << " us each ("
                      << static_cast<double>(actions) / evals << " actions each; checksum " << sink << ")\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
