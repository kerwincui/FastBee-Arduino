#include "./network/handlers/HandlerUtils.h"
#include <memory>

namespace HandlerUtils {

bool sendJsonListChunked(
    AsyncWebServerRequest* request,
    const String& header,
    std::vector<String>&& items,
    const char* footer)
{
    // 4 阶段状态机：0=header / 1=items / 2=footer / 3=done
    struct State {
        String header;
        size_t headerPos = 0;
        std::vector<String> items;
        size_t itemIdx = 0;
        size_t itemPos = 0;
        bool itemNeedsComma = false;
        const char* footer = "]}";
        size_t footerLen = 2;
        size_t footerPos = 0;
        uint8_t phase = 0;
    };

    auto state = std::make_shared<State>();
    state->header = header;
    state->items = std::move(items);
    state->footer = footer ? footer : "]}";
    state->footerLen = strlen(state->footer);

    AsyncWebServerResponse* resp = request->beginChunkedResponse(
        "application/json",
        [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            size_t written = 0;

            // Phase 0: header
            if (state->phase == 0) {
                size_t remain = state->header.length() - state->headerPos;
                size_t copy = remain < maxLen ? remain : maxLen;
                if (copy > 0) {
                    memcpy(buffer, state->header.c_str() + state->headerPos, copy);
                    state->headerPos += copy;
                    written += copy;
                }
                if (state->headerPos >= state->header.length()) {
                    // header 已发完，可以释放其内存
                    state->header = String();
                    state->phase = 1;
                }
                if (written >= maxLen) return written;
            }

            // Phase 1: items（逐项发送，发完释放）
            while (state->phase == 1 && written < maxLen) {
                if (state->itemIdx >= state->items.size()) {
                    state->phase = 2;
                    break;
                }

                // 项前的逗号分隔（除第一项外）
                if (state->itemPos == 0 && state->itemNeedsComma) {
                    if (written >= maxLen) return written;
                    buffer[written++] = ',';
                    state->itemNeedsComma = false;
                    if (written >= maxLen) return written;
                }

                String& item = state->items[state->itemIdx];
                size_t remain = item.length() - state->itemPos;
                size_t avail = maxLen - written;
                size_t copy = remain < avail ? remain : avail;
                if (copy > 0) {
                    memcpy(buffer + written, item.c_str() + state->itemPos, copy);
                    state->itemPos += copy;
                    written += copy;
                }
                if (state->itemPos >= item.length()) {
                    // 当前项发送完毕，立即释放该 String 的堆块
                    state->items[state->itemIdx] = String();
                    state->itemIdx++;
                    state->itemPos = 0;
                    state->itemNeedsComma = true;
                }
            }

            // Phase 2: footer
            if (state->phase == 2 && written < maxLen) {
                size_t remain = state->footerLen - state->footerPos;
                size_t avail = maxLen - written;
                size_t copy = remain < avail ? remain : avail;
                if (copy > 0) {
                    memcpy(buffer + written, state->footer + state->footerPos, copy);
                    state->footerPos += copy;
                    written += copy;
                }
                if (state->footerPos >= state->footerLen) {
                    state->phase = 3;
                    // 释放 items 容器内存
                    state->items.clear();
                    state->items.shrink_to_fit();
                }
            }

            // Phase 3: done — 返回 0 通知框架结束
            return written;
        });

    if (!resp) {
        return false;
    }
    request->send(resp);
    return true;
}

} // namespace HandlerUtils
