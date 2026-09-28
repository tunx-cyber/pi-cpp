#include "terminal_input.h"

#include <poll.h>
#include <unistd.h>

#include <cstdlib>

namespace pi::terminal
{
// ---------- UTF-8 与终端显示宽度 ----------

/** UTF-8 首字节对应的后续字节数（非法首字节返回 0）。 */
int utf8_continuation_bytes(unsigned char lead)
{
    if ((lead & 0xE0) == 0xC0) return 1;
    if ((lead & 0xF0) == 0xE0) return 2;
    if ((lead & 0xF8) == 0xF0) return 3;
    return 0;
}

/** Unicode 码点 → UTF-8 字节序列（CSI-u/kitty 协议上报非 ASCII 键时使用）。 */
std::string utf8_encode(uint32_t codepoint)
{
    std::string out;
    if (codepoint < 0x80)
    {
        out += static_cast<char>(codepoint);
    }
    else if (codepoint < 0x800)
    {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else if (codepoint < 0x10000)
    {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return out;
}

/** 近似 wcwidth 的宽字符判断（CJK 主区间 + 全角符号 + 常用 emoji，不依赖 locale）。 */
bool is_wide_codepoint(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) ||    // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0xA4CF) ||    // CJK 部首/汉字/假名/谚文
           (cp >= 0xAC00 && cp <= 0xD7A3) ||    // Hangul 音节
           (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK 兼容表意
           (cp >= 0xFE30 && cp <= 0xFE4F) ||    // CJK 兼容形式
           (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角形式
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||    // 全角符号
           (cp >= 0x1F300 && cp <= 0x1FAFF) ||  // 表情符号/符号
           (cp >= 0x20000 && cp <= 0x3FFFD);    // CJK 扩展 B+
}

/** 字符串的终端显示宽度（宽字符计 2 列），用于多行光标定位。 */
size_t display_width(const std::string& text)
{
    size_t width = 0;
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80)
        {
            width += 1;
            ++i;
            continue;
        }
        const int extra = utf8_continuation_bytes(c);
        if (extra <= 0 || i + extra >= text.size())
        {
            width += 1;  // 非法序列按 1 列兜底
            ++i;
            continue;
        }
        uint32_t cp = c & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k)
        {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        width += is_wide_codepoint(cp) ? 2 : 1;
        i += extra + 1;
    }
    return width;
}

size_t count_newlines(const std::string& text)
{
    size_t count = 0;
    for (char c : text)
    {
        if (c == '\n') ++count;
    }
    return count;
}

// ---------- 键盘输入解码 ----------

/** 带超时的单字节读取。raw mode 下裸按 Esc 只有 1 字节，直接阻塞读后续字节会挂起 UI。 */
bool read_byte_with_timeout(char& out, int timeout_ms)
{
    struct pollfd pfd
    {
        STDIN_FILENO, POLLIN, 0
    };
    if (poll(&pfd, 1, timeout_ms) <= 0) return false;
    return read(STDIN_FILENO, &out, 1) == 1;
}

/** stdin 在指定时间内是否有数据（用于流式中 Ctrl+C 二次确认）。 */
bool stdin_has_data(int timeout_ms)
{
    struct pollfd pfd
    {
        STDIN_FILENO, POLLIN, 0
    };
    return poll(&pfd, 1, timeout_ms) > 0;
}

/**
 * 解码后的按键事件。兼容普通终端字节流（CR/BS/ESC [ 序列）与
 * CSI-u/kitty 键盘协议（`CSI <codepoint> [;<modifiers>] u`，enter_raw_mode 时
 * 通过 `CSI > 1 u` 启用；不支持的终端忽略启用序列、继续发传统字节流）。
 */

KeyEvent arrow_key_event(char final_byte)
{
    KeyEvent key;
    switch (final_byte)
    {
        case 'A':
            key.type = KeyEvent::Type::Up;
            break;
        case 'B':
            key.type = KeyEvent::Type::Down;
            break;
        case 'C':
            key.type = KeyEvent::Type::Right;
            break;
        case 'D':
            key.type = KeyEvent::Type::Left;
            break;
        case 'H':
            key.type = KeyEvent::Type::Home;
            break;
        case 'F':
            key.type = KeyEvent::Type::End;
            break;
        default:
            break;
    }
    return key;
}

/** 解析 CSI-u 序列（形如 "13;1u"，不带前导 ESC [）。 */
KeyEvent decode_csi_u_sequence(const std::string& seq)
{
    KeyEvent key;
    const size_t semi = seq.find(';');
    const int code = std::atoi(seq.substr(0, semi).c_str());
    const int mods = semi == std::string::npos ? 0 : std::atoi(seq.substr(semi + 1).c_str());

    switch (code)
    {
        case 13:
            // kitty 协议 shift=1、alt=2；iTerm2 传统 CSI-u 用 2 表示 shift。
            // 两者都映射为换行（Alt+Enter 插换行同样是合理行为）。
            key.type = (mods & 3) ? KeyEvent::Type::ShiftEnter : KeyEvent::Type::Enter;
            return key;
        case 27:
            key.type = KeyEvent::Type::Esc;
            return key;
        case 9:
            key.type = KeyEvent::Type::Tab;
            return key;
        case 127:
            key.type = KeyEvent::Type::Backspace;
            return key;
        case 65:
            key.type = KeyEvent::Type::Up;
            return key;
        case 66:
            key.type = KeyEvent::Type::Down;
            return key;
        case 67:
            key.type = KeyEvent::Type::Right;
            return key;
        case 68:
            key.type = KeyEvent::Type::Left;
            return key;
        default:
            break;
    }
    if (mods & 4)  // Ctrl 修饰
    {
        if (code == 99 || code == 3)
            key.type = KeyEvent::Type::CtrlC;  // c
        else if (code == 108)
            key.type = KeyEvent::Type::CtrlL;  // l
        else if (code == 112)
            key.type = KeyEvent::Type::CtrlP;  // p
        else if (code == 106)
            key.type = KeyEvent::Type::ShiftEnter;  // Ctrl+J 换行兜底
        return key;
    }
    if (code >= 32)
    {
        key.type = KeyEvent::Type::Char;
        key.text = utf8_encode(static_cast<uint32_t>(code));
    }
    return key;
}

KeyEvent decode_key_event()
{
    KeyEvent key;
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return key;

    const auto char_event = [&](std::string text)
    {
        key.type = KeyEvent::Type::Char;
        key.text = std::move(text);
        return key;
    };

    switch (static_cast<unsigned char>(c))
    {
        case '\r':
        case '\n':  // 部分终端在 raw mode 下仍因 ICRNL 把 Enter 送到为 \n
            key.type = KeyEvent::Type::Enter;
            return key;
        case 3:
            key.type = KeyEvent::Type::CtrlC;
            return key;
        case 12:
            key.type = KeyEvent::Type::CtrlL;
            return key;
        case 16:
            key.type = KeyEvent::Type::CtrlP;
            return key;
        case 9:
            key.type = KeyEvent::Type::Tab;
            return key;
        case 127:
        case 8:
            key.type = KeyEvent::Type::Backspace;
            return key;
        case 27:
        {
            // Esc：可能是裸 Esc、SS3（Fn 方向键）、CSI 序列或 CSI-u/kitty 协议
            char next;
            if (!read_byte_with_timeout(next, 50))
            {
                key.type = KeyEvent::Type::Esc;
                return key;
            }
            if (next == '\r' || next == '\n')
            {
                key.type = KeyEvent::Type::ShiftEnter;  // Option+Enter：全终端可用的换行兜底
                return key;
            }
            if (next == 'O')  // SS3
            {
                char arrow;
                if (read_byte_with_timeout(arrow, 50)) return arrow_key_event(arrow);
                key.type = KeyEvent::Type::Esc;
                return key;
            }
            if (next != '[')
            {
                return char_event(std::string(1, next));  // Alt+字符（忽略修饰）
            }
            // CSI 序列：收集到终止字节（字母 / ~）
            std::string seq;
            while (seq.size() < 24)
            {
                char b;
                if (!read_byte_with_timeout(b, 50)) break;
                seq += b;
                if (b == 'u' || (b >= 'A' && b <= 'Z') || b == '~') break;
            }
            if (seq.empty())
            {
                key.type = KeyEvent::Type::Esc;
                return key;
            }
            if (seq.back() == 'u') return decode_csi_u_sequence(seq);
            return arrow_key_event(seq.back());  // 传统 CSI：A/B/C/D/H/F，其余忽略
        }
        default:
        {
            // UTF-8 多字节序列：读取完整字符
            std::string text(1, c);
            const int extra = utf8_continuation_bytes(static_cast<unsigned char>(c));
            for (int i = 0; i < extra; ++i)
            {
                char b;
                if (!read_byte_with_timeout(b, 50)) break;
                text += b;
            }
            return char_event(std::move(text));
        }
    }
}

}  // namespace pi::terminal
