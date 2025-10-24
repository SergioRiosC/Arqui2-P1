#include "parser.h"
#include <sstream>
#include <cctype>
#include <algorithm>

// ---------------- FUNCIONES AUXILIARES ----------------

// Elimina espacios en blanco al inicio y final
static inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Divide una línea en tokens (separados por espacios o comas)
static std::vector<std::string> tokenize_line(const std::string &line) {
    std::vector<std::string> toks;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == ',') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            continue;
        }
        if (isspace((unsigned char)c)) {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

// Verifica si un token es un registro (R0-R7)
static bool is_register_token(const std::string &tok, int &reg_out) {
    if (tok.size() >= 2 && (tok[0] == 'R' || tok[0] == 'r')) {
        try {
            int r = std::stoi(tok.substr(1));
            if (r >= 0 && r < 8) { reg_out = r; return true; }
        } catch(...) {}
    }
    return false;
}

// Convierte string a número (soporta decimal y hexadecimal)
static bool parse_number(const std::string &tok, size_t &out) {
    try {
        long long v = std::stoll(tok);
        if (v < 0) return false;
        out = (size_t)v;
        return true;
    } catch(...) { return false; }
}

// CONSTRUYE INSTRUCCIÓN A PARTIR DE TOKENS
static Instr make_instr_from_tokens(const std::vector<std::string> &toks) {
    Instr I;
    if (toks.empty()) return I;
    std::string op = toks[0];
    std::transform(op.begin(), op.end(), op.begin(), ::toupper);

    // PARSEO DE CADA TIPO DE INSTRUCCIÓN
    if (op == "LOAD") {
        I.op = OpCode::LOAD;
        if (toks.size() < 3) return I;
        int rd;
        if (is_register_token(toks[1], rd)) I.rd = rd;
        std::string operand = toks[2];
        // Detecta si la dirección es directa o por registro
        if (operand.size() >= 3 && operand.front()=='[' && operand.back()==']') {
            std::string inner = operand.substr(1, operand.size()-2);
            int r;
            if (is_register_token(inner, r)) { I.addr_is_reg = true; I.ra = r; }
        } else {
            size_t addr;
            if (parse_number(operand, addr)) { I.addr_is_reg = false; I.address = addr; }
        }
    }
    else if (op == "STORE") {
        I.op = OpCode::STORE;
        if (toks.size() < 3) return I;
        int rd;
        if (is_register_token(toks[1], rd)) I.rd = rd;
        std::string operand = toks[2];
        if (operand.size() >= 3 && operand.front()=='[' && operand.back()==']') {
            std::string inner = operand.substr(1, operand.size()-2);
            int r;
            if (is_register_token(inner, r)) { I.addr_is_reg = true; I.ra = r; }
        } else {
            size_t addr;
            if (parse_number(operand, addr)) { I.addr_is_reg = false; I.address = addr; }
        }
    }
    else if (op == "FMUL" || op == "FADD") {
        I.op = (op=="FMUL" ? OpCode::FMUL : OpCode::FADD);
        if (toks.size() < 4) return I;
        int rd, ra, rb;
        if (is_register_token(toks[1], rd) &&
            is_register_token(toks[2], ra) &&
            is_register_token(toks[3], rb)) {
            I.rd = rd; I.ra = ra; I.rb = rb;
        }
    }
    else if (op == "INC" || op == "DEC") {
        I.op = (op=="INC" ? OpCode::INC : OpCode::DEC);
        if (toks.size() >= 2) {
            int r; if (is_register_token(toks[1], r)) I.rd = r;
        }
    }
    else if (op == "JNZ") {
        I.op = OpCode::JNZ;
        if (toks.size() >= 2) {
            // Dos formatos: "JNZ R3, LOOP" o "JNZ LOOP" (registro implícito R3)
            int r;
            if (is_register_token(toks[1], r)) {
                I.rd = r;
                if (toks.size() >= 3) I.label = toks[2];
            } else {
                I.rd = 3; // Registro por defecto para contador
                I.label = toks[1];
            }
        }
    }
    else if (op == "HALT") {
        I.op = OpCode::HALT;
    } else {
        I.op = OpCode::NOP; // No operation
    }
    return I;
}

// FUNCIÓN PRINCIPAL DE PARSING
void parse_asm(const std::string &asm_text,
               std::vector<Instr> &out_program,
               std::unordered_map<std::string,size_t> &out_label_map) {
    out_program.clear(); out_label_map.clear();
    std::istringstream ss(asm_text); 
    std::string line;
    std::vector<std::string> cleaned;
    
    // PRIMERA PASADA: Limpieza y detección de etiquetas
    while (std::getline(ss, line)) {
        // Eliminar comentarios
        size_t pos = line.find("//");
        if (pos != std::string::npos) line = line.substr(0, pos);
        pos = line.find('#');
        if (pos != std::string::npos) line = line.substr(0, pos);
        
        line = trim(line);
        if (line.empty()) continue;
        
        // Detectar etiquetas (terminan con ':')
        if (!line.empty() && line.back() == ':') {
            std::string lab = trim(line.substr(0, line.size()-1));
            out_label_map[lab] = cleaned.size(); // Guardar posición de la etiqueta
            continue;
        }
        cleaned.push_back(line);
    }
    
    // SEGUNDA PASADA: Convertir líneas limpias a instrucciones
    for (const auto &ln : cleaned) {
        auto toks = tokenize_line(ln);
        if (toks.empty()) continue;
        out_program.push_back(make_instr_from_tokens(toks));
    }
}