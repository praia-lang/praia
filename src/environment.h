#pragma once

#include "value.h"
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

class Environment : public std::enable_shared_from_this<Environment> {
    friend class GcHeap;        // GC needs access to variables and parent for mark/sweep
    friend class Interpreter;   // tree-walker async deep-copy iterates variables
public:
    Environment() = default;
    explicit Environment(std::shared_ptr<Environment> parent)
        : parent(std::move(parent)) {}

    void define(const std::string& name, Value value) {
        variables[name] = std::move(value);
    }

    Value get(const std::string& name, int line) const {
        auto it = variables.find(name);
        if (it != variables.end()) return it->second;
        if (parent) return parent->get(name, line);
        throw RuntimeError("Undefined variable '" + name + "'", line);
    }

    void set(const std::string& name, Value value, int line) {
        auto it = variables.find(name);
        if (it != variables.end()) {
            if (isConstantName(name))
                std::cerr << formatLocation(line) << " Warning: reassigning constant '" << name << "'" << std::endl;
            it->second = std::move(value);
            return;
        }
        if (parent) { parent->set(name, std::move(value), line); return; }
        throw RuntimeError("Undefined variable '" + name + "'", line);
    }

    static bool isConstantName(const std::string& name) {
        if (name.size() < 2) return false;
        bool hasLetter = false;
        for (char c : name) {
            if (c >= 'A' && c <= 'Z') hasLetter = true;
            else if (c != '_' && !(c >= '0' && c <= '9')) return false;
        }
        return hasLetter;
    }

private:
    std::unordered_map<std::string, Value> variables;
    std::shared_ptr<Environment> parent;
};
