// Note templates: each maps a language to the system prompt that shapes the
// summary. Users can override any of these, or add their own, from Settings.
#pragma once

#include <string>
#include <vector>

namespace transcriptor::llm {

struct TemplateLabel {
    std::string value;   // "meeting", "standup", ...
    std::string label;   // localized display name
};

// Template ids in menu order.
const std::vector<std::string>& template_ids();

bool is_template(const std::string& id);

// The built-in system prompt. Unknown ids fall back to "meeting", unknown
// languages to English.
std::string system_prompt(const std::string& template_id,
                          const std::string& language);

// Dropdown entries for `language`.
std::vector<TemplateLabel> template_labels(const std::string& language);

}  // namespace transcriptor::llm
