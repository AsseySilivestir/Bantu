#pragma once
/**
 * Bantu Language - Variable Environment (Scope Chain)
 */

#include "types.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>

class Environment {
public:
    std::unordered_map<std::string, Value> variables;
    std::unordered_set<std::string> constNames;  // names declared `const` in THIS scope
    std::shared_ptr<Environment> parent;

    explicit Environment(std::shared_ptr<Environment> parentEnv = nullptr)
        : parent(std::move(parentEnv)) {}

    // define() creates/overwrites a binding in this scope. When isConst is true
    // the binding becomes final — a later assignment to it raises an error
    // (like Java's `final`; the referenced object may still be mutated).
    void define(const std::string& name, const Value& value, bool isConst = false) {
        variables[name] = value;
        if (isConst) constNames.insert(name);
    }

    Value get(const std::string& name) {
        auto it = variables.find(name);
        if (it != variables.end()) return it->second;
        if (parent) return parent->get(name);
        ErrorHandler::throwReferenceError("Undefined variable: " + name);
        return Value();
    }

    Value& getRef(const std::string& name) {
        auto it = variables.find(name);
        if (it != variables.end()) return it->second;
        if (parent) return parent->getRef(name);
        ErrorHandler::throwReferenceError("Undefined variable: " + name);
        static Value nullVal;
        return nullVal;
    }

    void set(const std::string& name, const Value& value) {
        auto it = variables.find(name);
        if (it != variables.end()) {
            if (constNames.count(name)) {
                ErrorHandler::throwError("Cannot reassign constant '" + name + "'",
                                         0, 0, ErrorHandler::TYPE_ERROR);
            }
            it->second = value;
            return;
        }
        if (parent) { parent->set(name, value); return; }
        ErrorHandler::throwReferenceError("Undefined variable: " + name);
    }

    bool has(const std::string& name) const {
        auto it = variables.find(name);
        if (it != variables.end()) return true;
        if (parent) return parent->has(name);
        return false;
    }
};
