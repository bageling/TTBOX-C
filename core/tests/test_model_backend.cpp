#include <iostream>
#include <string>
#include "model/backend/ModelBackend.hpp"

using namespace ttbox::core;

int main() {
    if (ModelBackendFactory::detect_kind("demo.onnx") != ModelBackendKind::kOnnx) return 1;
    if (ModelBackendFactory::detect_kind("demo.rknn") != ModelBackendKind::kRknn) return 2;
    if (ModelBackendFactory::detect_kind("demo.bin") != ModelBackendKind::kAuto) return 3;
    std::string error;
    auto backend = ModelBackendFactory::create(ModelBackendKind::kOnnx, &error);
    if (!backend || backend->initialized()) return 4;
    if (error.empty()) return 5;
    std::vector<std::vector<float>> outputs;
    if (backend->infer(nullptr, 0, outputs, &error)) return 6;
    std::cout << "model backend abstraction: PASS\n";
    return 0;
}
