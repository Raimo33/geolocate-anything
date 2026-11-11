// CLI11's non-template code compiled once, here (CLI11_COMPILE), instead of inline in
// main.cpp on every rebuild of it.
#include <CLI/CLI.hpp>
#include <CLI/impl/App_inl.hpp>
#include <CLI/impl/Argv_inl.hpp>
#include <CLI/impl/Config_inl.hpp>
#include <CLI/impl/Encoding_inl.hpp>
#if __has_include(<CLI/impl/ExtraValidators_inl.hpp>)  // CLI11 >= 2.5
#include <CLI/impl/ExtraValidators_inl.hpp>
#endif
#include <CLI/impl/Formatter_inl.hpp>
#include <CLI/impl/Option_inl.hpp>
#include <CLI/impl/Split_inl.hpp>
#include <CLI/impl/StringTools_inl.hpp>
#include <CLI/impl/Validators_inl.hpp>
