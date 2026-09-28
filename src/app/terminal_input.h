#pragma once

#include <cstdint>

#include <string>

namespace pi::terminal
{
struct KeyEvent
{
    enum class Type
    {
        None,
        Char,  // text 为待插入的字节序列
        Enter,
        ShiftEnter,  // 换行：Shift+Enter（CSI-u）/ Ctrl+J / Option+Enter
        Esc,
        CtrlC,
        CtrlL,
        CtrlP,
        Tab,
        Backspace,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
    };
    Type type = Type::None;
    std::string text;
};

size_t display_width(const std::string& text);
size_t count_newlines(const std::string& text);
bool stdin_has_data(int timeout_ms);
KeyEvent decode_key_event();
}  // namespace pi::terminal
