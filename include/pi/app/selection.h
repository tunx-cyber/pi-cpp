#pragma once

#include <cstddef>

namespace pi
{

/** 列表可见窗口（/ 命令菜单与 /model 选择器共用同一套滚动规则）。 */
struct SelectionWindow
{
    std::size_t offset = 0;   // 窗口内第一条在完整列表中的下标
    std::size_t visible = 0;  // 窗口内条目数（<= max_visible）
};

/** 把选中下标夹到 [0, total) ；total 为 0 时返回 0。 */
inline std::size_t clamp_selection(std::size_t index, std::size_t total)
{
    if (total == 0) return 0;
    return index < total ? index : total - 1;
}

/**
 * 计算让 selected 始终可见的窗口：优先让选中项出现在窗口内，
 * 窗口尽量填满且不越过列表末尾。
 */
inline SelectionWindow selection_window(std::size_t total, std::size_t selected,
                                        std::size_t max_visible)
{
    SelectionWindow window;
    if (total == 0 || max_visible == 0) return window;

    const std::size_t index = clamp_selection(selected, total);
    window.visible = total < max_visible ? total : max_visible;
    if (index >= window.visible)
    {
        window.offset = index - window.visible + 1;
        if (window.offset + window.visible > total) window.offset = total - window.visible;
    }
    return window;
}

}  // namespace pi
