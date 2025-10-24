#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>
#include <unordered_map>
#include "instr.h"

// PARSER DE ENSAMBLADOR
void parse_asm(const std::string &asm_text, std::vector<Instr> &out_program, std::unordered_map<std::string,size_t> &out_label_map);
#endif
