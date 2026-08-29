#include "llm/templates.h"

#include <map>

namespace transcriptor::llm {

namespace {

struct Prompts {
    const char* tr;
    const char* en;
};

const std::map<std::string, Prompts>& prompts() {
    static const std::map<std::string, Prompts> kPrompts = {
        {"meeting", {
            "Sen bir toplantı asistanısın. Sana konuşmanın metne dökülmüş hali "
            "verilecek. Türkçe bir toplantı tutanağı çıkar. Yapı:\n"
            "• Özet: 2-4 cümle.\n"
            "• Ana Noktalar: madde madde.\n"
            "• Kararlar: alınan kararlar, yoksa 'yok'.\n"
            "• Aksiyonlar / Yapılacaklar: mümkünse sorumlu kişiyle, yoksa 'yok'.\n"
            "Sadıksın, metinde olmayan bilgi uydurmazsın.",

            "You are a meeting assistant. You receive a transcript. Produce "
            "meeting minutes. Structure:\n"
            "• Summary: 2-4 sentences.\n"
            "• Key Points: bullets.\n"
            "• Decisions: decisions made, or 'none'.\n"
            "• Action Items: with owner where possible, or 'none'.\n"
            "Be faithful; do not invent facts."}},

        {"standup", {
            "Sen bir günlük standup asistanısın. Konuşmayı kişilere göre özetle. "
            "Her konuşmacı için:\n"
            "• Dün / Yapılanlar\n• Bugün / Planlananlar\n• Engeller (varsa)\n"
            "Kısa ve madde madde yaz. Metinde olmayanı uydurma.",

            "You are a daily standup assistant. Summarize per person. For each "
            "speaker:\n• Yesterday / Done\n• Today / Planned\n• Blockers (if any)\n"
            "Keep it short and bulleted. Do not invent facts."}},

        {"lecture", {
            "Sen bir ders/sunum notu asistanısın. Türkçe ders notu çıkar. Yapı:\n"
            "• Konu: tek cümle.\n• Ana Kavramlar: madde madde tanımlarla.\n"
            "• Önemli Noktalar: madde madde.\n• Örnekler: varsa.\n"
            "• Sorular / Belirsizlikler: varsa.\nMetne sadık kal.",

            "You are a lecture/talk notes assistant. Produce study notes. "
            "Structure:\n• Topic: one sentence.\n• Key Concepts: bullets with "
            "definitions.\n• Important Points: bullets.\n• Examples: if any.\n"
            "• Questions / Unclear: if any.\nStay faithful to the text."}},

        {"interview", {
            "Sen bir görüşme/mülakat asistanısın. Görüşmeyi özetle. Yapı:\n"
            "• Özet: 2-3 cümle.\n• Ana Sorular ve Yanıtlar: madde madde.\n"
            "• Öne Çıkanlar / İçgörüler: madde madde.\n"
            "• Takip Edilecekler: varsa.\nMetinde olmayanı uydurma.",

            "You are an interview assistant. Summarize the conversation. "
            "Structure:\n• Summary: 2-3 sentences.\n• Key Questions & Answers: "
            "bullets.\n• Highlights / Insights: bullets.\n• Follow-ups: if any.\n"
            "Do not invent facts."}},

        {"general", {
            "Sana bir ses kaydının metni verilecek. Türkçe özetle. Yapı:\n"
            "• Özet: 2-4 cümle.\n• Ana Noktalar: madde madde.\n"
            "• Aksiyonlar: varsa madde madde, yoksa 'yok'.\n"
            "Sadıksın, metinde olmayan bilgi uydurmazsın.",

            "You receive an audio transcript. Summarize it. Structure:\n"
            "• Summary: 2-4 sentences.\n• Key Points: bullets.\n"
            "• Action Items: bullets or 'none'.\nBe faithful; do not invent facts."}},
    };
    return kPrompts;
}

const std::map<std::string, Prompts>& labels() {
    static const std::map<std::string, Prompts> kLabels = {
        {"meeting",   {"Toplantı Tutanağı", "Meeting Minutes"}},
        {"standup",   {"Günlük Standup",    "Daily Standup"}},
        {"lecture",   {"Ders / Sunum Notu", "Lecture Notes"}},
        {"interview", {"Görüşme / Mülakat", "Interview"}},
        {"general",   {"Genel Özet",        "General Summary"}},
    };
    return kLabels;
}

const char* pick(const Prompts& p, const std::string& language) {
    return (language == "tr") ? p.tr : p.en;
}

}  // namespace

const std::vector<std::string>& template_ids() {
    static const std::vector<std::string> kIds = {"meeting", "standup", "lecture",
                                                  "interview", "general"};
    return kIds;
}

bool is_template(const std::string& id) {
    return prompts().find(id) != prompts().end();
}

std::string system_prompt(const std::string& template_id,
                          const std::string& language) {
    auto it = prompts().find(template_id);
    if (it == prompts().end()) it = prompts().find("meeting");
    return pick(it->second, language);
}

std::vector<TemplateLabel> template_labels(const std::string& language) {
    std::vector<TemplateLabel> out;
    for (const std::string& id : template_ids()) {
        auto it = labels().find(id);
        if (it == labels().end()) continue;
        out.push_back({id, pick(it->second, language)});
    }
    return out;
}

}  // namespace transcriptor::llm
