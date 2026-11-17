#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imnodes.h"
#include <GLFW/glfw3.h>
#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Record {
    std::vector<std::string> fields;
    std::string              source;
};

struct Database {
    std::vector<std::string> columns;
    std::vector<Record>      rows;
};

struct Edge { int a = 0; int b = 0; };

struct GraphState {
    std::unordered_map<int, ImVec2> positions;
    std::vector<Edge>               edges;
};

static std::vector<std::string> ParseCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else inQuotes = false;
            } else cur += c;
        } else {
            if (c == '"') inQuotes = true;
            else if (c == ',') { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static bool EnsureDataFolder(const fs::path& dir, std::string& err) {
    std::error_code ec;
    if (fs::exists(dir)) return true;
    if (!fs::create_directories(dir, ec) || ec) {
        err = "Не удалось создать папку data: " + ec.message();
        return false;
    }
    return true;
}

static Database LoadDatabase(const fs::path& dir, std::string& err) {
    Database db;
    if (!fs::exists(dir)) { err = "Папка data отсутствует"; return db; }
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".csv")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    bool headerDone = false;
    for (auto& f : files) {
        std::ifstream in(f);
        if (!in) { err += "Не открыт: " + f.string() + "\n"; continue; }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto cells = ParseCsvLine(line);
            if (!headerDone) { db.columns = cells; headerDone = true; }
            else if (cells.size() == db.columns.size()) {
                Record r;
                r.fields = std::move(cells);
                r.source = f.filename().string();
                db.rows.push_back(std::move(r));
            }
        }
    }
    return db;
}

struct BirthInfo {
    bool valid = false;
    int  year = 0, month = 0, day = 0;
    int  age  = 0;
};

static BirthInfo ParseBirthDate(const std::string& s,
                                int currentYear, int currentMonth, int currentDay) {
    BirthInfo b;
    if (s.empty()) return b;

    int y = 0, m = 0, d = 0;
    if (sscanf_s(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3) {}
    else if (sscanf_s(s.c_str(), "%d.%d.%d", &d, &m, &y) == 3) {}
    else if (sscanf_s(s.c_str(), "%d/%d/%d", &d, &m, &y) == 3) {}
    else if (sscanf_s(s.c_str(), "%d-%d-%d", &d, &m, &y) == 3) {}
    else if (s.size() == 8 && std::all_of(s.begin(), s.end(), ::isdigit)) {
        y = std::stoi(s.substr(0, 4));
        m = std::stoi(s.substr(4, 2));
        d = std::stoi(s.substr(6, 2));
    } else {
        return b;
    }

    if (y < 1900 || y > currentYear) return b;
    if (m < 1 || m > 12) return b;
    if (d < 1 || d > 31) return b;

    int age = currentYear - y;
    if (currentMonth < m || (currentMonth == m && currentDay < d)) --age;
    if (age < 0) age = 0;

    b.valid = true;
    b.year = y; b.month = m; b.day = d;
    b.age  = age;
    return b;
}

static int FindAgeColumn(const Database& db) {
    static const char* keys[] = {"age", "birth", "birthdate", "dob",
                                 "др", "дата рождения", "рождения"};
    for (int c = 0; c < (int)db.columns.size(); ++c) {
        std::string lc = ToLower(db.columns[c]);
        for (auto k : keys)
            if (lc.find(k) != std::string::npos) return c;
    }
    return -1;
}

struct SearchHit {
    int              rowIndex = 0;
    std::vector<int> matchedColumns;
};

static std::vector<SearchHit> SearchEx(const Database& db, const std::string& query) {
    std::vector<SearchHit> hits;
    if (query.empty()) return hits;
    std::string q = ToLower(query);
    for (int i = 0; i < (int)db.rows.size(); ++i) {
        SearchHit h;
        h.rowIndex = i;
        for (int c = 0; c < (int)db.rows[i].fields.size(); ++c)
            if (ToLower(db.rows[i].fields[c]).find(q) != std::string::npos)
                h.matchedColumns.push_back(c);
        if (!h.matchedColumns.empty()) hits.push_back(std::move(h));
    }
    return hits;
}

static void BuildAutoEdges(const Database& db, const std::vector<int>& hits,
                           GraphState& g, int& outEdges) {
    static const char* keys[] = {"mail", "phone", "тел", "номер", "nick"};
    std::vector<int> keyCols;
    for (int c = 0; c < (int)db.columns.size(); ++c) {
        std::string lc = ToLower(db.columns[c]);
        for (auto k : keys)
            if (lc.find(k) != std::string::npos) { keyCols.push_back(c); break; }
    }
    if (keyCols.empty()) { outEdges = 0; return; }
    std::unordered_map<std::string, std::vector<int>> buckets;
    for (int hi = 0; hi < (int)hits.size(); ++hi) {
        auto& r = db.rows[hits[hi]];
        for (int c : keyCols) {
            if (r.fields[c].empty()) continue;
            buckets[ToLower(r.fields[c])].push_back(hi);
        }
    }
    int added = 0;
    for (auto& [val, lst] : buckets)
        for (size_t i = 0; i < lst.size(); ++i)
            for (size_t j = i + 1; j < lst.size(); ++j) {
                bool exists = false;
                for (auto& e : g.edges)
                    if ((e.a == lst[i] && e.b == lst[j]) ||
                        (e.a == lst[j] && e.b == lst[i])) { exists = true; break; }
                if (!exists) { g.edges.push_back({lst[i], lst[j]}); ++added; }
            }
    outEdges = added;
}

static void SaveGraph(const fs::path& file, const GraphState& g) {
    std::ofstream o(file);
    if (!o) return;
    for (auto& e : g.edges) o << e.a << ' ' << e.b << '\n';
}

static void LoadGraph(const fs::path& file, GraphState& g) {
    std::ifstream in(file);
    if (!in) return;
    int a, b;
    g.edges.clear();
    while (in >> a >> b) g.edges.push_back({a, b});
}

static std::string RunCommandAndCapture(const std::string& command) {
    std::string result;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "Error: CreatePipe failed";

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    PROCESS_INFORMATION pi = {};

    std::string cmdLine = "cmd.exe /c " + command;

    if (!CreateProcessA(NULL, (LPSTR)cmdLine.c_str(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        return "Error: CreateProcess failed";
    }
    CloseHandle(hWrite);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        result += buffer;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return result;
}

struct ModuleInfo {
    std::string name;
    std::string pipPackage;
    std::string exeName;
    bool        installed = false;
};

struct ModuleManager {
    fs::path root;
    std::vector<ModuleInfo> mods;

    explicit ModuleManager(const fs::path& base) {
        root = base / "modules";
        mods = {
            {"Sherlock", "sherlock-project", "sherlock.exe", false},
            {"Holehe",   "holehe",           "holehe.exe",   false},
            {"Maigret",  "maigret",          "maigret.exe",  false},
        };
    }

    bool EnsureRoot(std::string& err) {
        std::error_code ec;
        if (fs::exists(root)) return true;
        if (!fs::create_directories(root, ec) || ec) {
            err = "Не удалось создать modules: " + ec.message();
            return false;
        }
        return true;
    }

    void RefreshInstalled() {
        for (auto& m : mods)
            m.installed = fs::exists(root / "Scripts" / m.exeName);
    }

    static std::string FindPython() {
        const char* candidates[] = {"python.exe", "py.exe"};
        for (auto c : candidates) {
            std::string test = std::string(c) + " --version >nul 2>&1";
            if (std::system(test.c_str()) == 0) return c;
        }
        return "";
    }

    std::string BuildInstallCommand(const ModuleInfo& m) const {
        std::string py = FindPython();
        if (py.empty()) return "";
        return py + " -m pip install --target=\"" + root.string() +
               "\" --upgrade " + m.pipPackage;
    }
};

enum class Screen { Loading, Main };

struct AppState {
    Screen      screen = Screen::Loading;
    std::string loadLog;
    float       loadProgress = 0.f;

    Database               db;
    std::vector<SearchHit> hits;
    int                    selected  = -1;
    int                    ageColumn = -1;
    char                   query[256] = {};
    std::string            status = "Готово";

    GraphState       graph;
    bool             graphEditMode = false;
    int              pendingFirstNode = -1;
    std::vector<int> nodeIds;

    char           osintTarget[256] = "";
    std::string    osintStatus      = "Готов";
    bool           osintRunning     = false;
    std::string    osintLog;
    ModuleManager* modules = nullptr;
};

static void ApplyDarkTheme() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    s.WindowRounding    = 6.0f;
    s.FrameRounding     = 5.0f;
    s.GrabRounding      = 5.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 6);
}

static void DrawLoadingScreen(AppState& st) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, 120));
    ImGui::SetCursorPosX(w * 0.5f - 180);
    ImGui::Text("SearchHelper");
    ImGui::SetCursorPosX(w * 0.5f - 180);
    ImGui::TextDisabled("Инициализация...");
    ImGui::Dummy(ImVec2(0, 20));
    ImGui::SetCursorPosX(w * 0.5f - 200);
    ImGui::ProgressBar(st.loadProgress, ImVec2(400, 8), "");
    ImGui::Dummy(ImVec2(0, 15));
    ImGui::SetCursorPosX(w * 0.5f - 200);
    ImGui::BeginChild("log", ImVec2(400, 160), true);
    ImGui::TextUnformatted(st.loadLog.c_str());
    ImGui::EndChild();
    ImGui::End();
}

static void DrawTopBar(AppState& st) {
    ImGui::BeginChild("topbar", ImVec2(0, 70), true);
    ImGui::TextUnformatted("SearchHelper");
    ImGui::SameLine();
    ImGui::TextDisabled("| OSINT");

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 240);
    bool enter = ImGui::InputTextWithHint("##q", "Введите запрос и нажмите Найти...",
                                          st.query, sizeof(st.query),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Найти", ImVec2(90, 0)) || enter) {
        st.hits      = SearchEx(st.db, st.query);
        st.ageColumn = FindAgeColumn(st.db);
        st.selected  = st.hits.empty() ? -1 : 0;
        st.status    = "Найдено: " + std::to_string(st.hits.size());

        st.nodeIds.clear();
        for (int i = 0; i < (int)st.hits.size(); ++i) {
            int id = i + 1;
            st.nodeIds.push_back(id);
            if (!st.graph.positions.count(id))
                st.graph.positions[id] = ImVec2(50 + (i % 8) * 180,
                                                50 + (i / 8) * 110);
        }
        std::vector<int> rowIdx;
        rowIdx.reserve(st.hits.size());
        for (auto& h : st.hits) rowIdx.push_back(h.rowIndex);
        int added = 0;
        BuildAutoEdges(st.db, rowIdx, st.graph, added);
        if (added) st.status += " (автосвязей: " + std::to_string(added) + ")";
    }
    ImGui::SameLine();
    if (ImGui::Button("Перезагрузить", ImVec2(120, 0))) {
        std::string err;
        st.db = LoadDatabase("data", err);
        st.hits.clear(); st.selected = -1;
        st.graph.edges.clear(); st.graph.positions.clear();
        st.status = "Загружено записей: " + std::to_string(st.db.rows.size());
        if (!err.empty()) st.status += " | " + err;
    }
    ImGui::EndChild();
}

static void DrawTable(AppState& st) {
    ImGui::BeginChild("table", ImVec2(0, 0), true);
    ImGui::TextDisabled("Результаты (%d)", (int)st.hits.size());
    ImGui::Separator();

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp;

    time_t t = time(nullptr);
    tm now{};
    localtime_s(&now, &t);

    if (ImGui::BeginTable("results", (int)st.db.columns.size(), flags)) {
        for (auto& c : st.db.columns) ImGui::TableSetupColumn(c.c_str());
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)st.hits.size(); ++i) {
            auto& h = st.hits[i];
            auto& r = st.db.rows[h.rowIndex];
            ImGui::TableNextRow();
            for (int c = 0; c < (int)r.fields.size(); ++c) {
                ImGui::TableSetColumnIndex(c);

                std::string display = r.fields[c];
                if (c == st.ageColumn) {
                    auto b = ParseBirthDate(r.fields[c], now.tm_year + 1900,
                                            now.tm_mon + 1, now.tm_mday);
                    if (b.valid)
                        display += "  (" + std::to_string(b.age) + " лет)";
                }

                bool matched = std::find(h.matchedColumns.begin(),
                                         h.matchedColumns.end(), c)
                               != h.matchedColumns.end();
                if (matched)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(1.0f, 0.85f, 0.3f, 1.0f));

                bool sel = (st.selected == i);
                if (ImGui::Selectable(display.c_str(), sel,
                                      ImGuiSelectableFlags_SpanAllColumns))
                    st.selected = i;

                if (matched) ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static void DrawDetails(AppState& st) {
    ImGui::BeginChild("details", ImVec2(0, 0), true);
    ImGui::TextUnformatted("Детали записи");
    ImGui::Separator();

    time_t t = time(nullptr);
    tm now{};
    localtime_s(&now, &t);

    if (st.selected >= 0 && st.selected < (int)st.hits.size()) {
        auto& h = st.hits[st.selected];
        auto& r = st.db.rows[h.rowIndex];

        for (int c = 0; c < (int)st.db.columns.size(); ++c) {
            ImGui::TextDisabled("%s", st.db.columns[c].c_str());
            std::string val = r.fields[c];
            if (c == st.ageColumn) {
                auto b = ParseBirthDate(r.fields[c], now.tm_year + 1900,
                                        now.tm_mon + 1, now.tm_mday);
                if (b.valid)
                    val += "   →   " + std::to_string(b.age) + " лет";
            }
            ImGui::TextWrapped("%s", val.c_str());
            ImGui::Spacing();
        }

        ImGui::Separator();
        if (!h.matchedColumns.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                               "Совпадения (%d):",
                               (int)h.matchedColumns.size());
            for (int c : h.matchedColumns) {
                ImGui::Bullet(); ImGui::SameLine();
                ImGui::Text("%s = %s",
                            st.db.columns[c].c_str(),
                            r.fields[c].c_str());
            }
            if (h.matchedColumns.size() >= 2)
                ImGui::TextDisabled("Найдено два и более совпадения в одной записи.");
            ImGui::Spacing();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Источник: %s", r.source.c_str());
    } else {
        ImGui::TextDisabled("Выберите запись в таблице");
    }
    ImGui::EndChild();
}

static void DrawGraph(AppState& st) {
    ImGui::BeginChild("graph", ImVec2(0, 0), true);
    if (ImGui::Button(st.graphEditMode ? "Режим добавления: ВКЛ" : "Добавить связь")) {
        st.graphEditMode = !st.graphEditMode;
        st.pendingFirstNode = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Сохранить граф")) SaveGraph("data/graph.txt", st.graph);
    ImGui::SameLine();
    if (ImGui::Button("Загрузить граф")) LoadGraph("data/graph.txt", st.graph);
    ImGui::SameLine();
    if (ImGui::Button("Очистить")) { st.graph.edges.clear(); st.pendingFirstNode = -1; }
    ImGui::SameLine();
    ImGui::TextDisabled("ЛКМ — тащить узел, ПКМ по ребру — удалить");

    if (st.graphEditMode) {
        ImGui::TextColored(ImVec4(1,0.8f,0.3f,1),
            "Режим добавления: кликните по двум узлам, чтобы соединить их");
        if (st.pendingFirstNode >= 0)
            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1),
                "Первый узел: #%d. Выберите второй.", st.pendingFirstNode);
    }
    ImGui::Separator();

    ImNodes::BeginNodeEditor();
    for (int i = 0; i < (int)st.nodeIds.size(); ++i) {
        int id = st.nodeIds[i];
        auto& r = st.db.rows[st.hits[i].rowIndex];
        ImNodes::BeginNode(id);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(st.db.columns.empty() ? "record"
                                                     : r.fields[0].c_str());
        ImNodes::EndNodeTitleBar();
        for (int c = 1; c < (int)st.db.columns.size(); ++c) {
            ImGui::TextDisabled("%s", st.db.columns[c].c_str());
            ImGui::TextWrapped("%s", r.fields[c].c_str());
        }
        ImNodes::EndNode();
    }

    int edgeToDelete = -1;
    for (int ei = 0; ei < (int)st.graph.edges.size(); ++ei) {
        auto& e = st.graph.edges[ei];
        ImNodes::Link(ei + 1000, e.a, e.b);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImVec2 pa = st.graph.positions[e.a];
            ImVec2 pb = st.graph.positions[e.b];
            ImVec2 mid = ImVec2((pa.x + pb.x) * 0.5f + 120,
                                (pa.y + pb.y) * 0.5f + 30);
            ImVec2 m = ImGui::GetMousePos();
            float dx = m.x - mid.x, dy = m.y - mid.y;
            if (dx*dx + dy*dy < 20.f * 20.f) edgeToDelete = ei;
        }
    }
    if (edgeToDelete >= 0) {
        st.graph.edges.erase(st.graph.edges.begin() + edgeToDelete);
        st.status = "Связь удалена";
    }
    ImNodes::EndNodeEditor();

    if (st.graphEditMode && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        int clicked = -1;
        for (int i = 0; i < (int)st.nodeIds.size(); ++i) {
            int id = st.nodeIds[i];
            ImVec2 p = ImNodes::GetNodeScreenSpacePos(id);
            ImVec2 s = ImNodes::GetNodeDimensions(id);
            if (mouse.x >= p.x && mouse.x <= p.x + s.x &&
                mouse.y >= p.y && mouse.y <= p.y + s.y) { clicked = id; break; }
        }
        if (clicked >= 0) {
            if (st.pendingFirstNode < 0) st.pendingFirstNode = clicked;
            else if (st.pendingFirstNode != clicked) {
                bool exists = false;
                for (auto& e : st.graph.edges)
                    if ((e.a == st.pendingFirstNode && e.b == clicked) ||
                        (e.a == clicked && e.b == st.pendingFirstNode)) { exists = true; break; }
                if (!exists) {
                    st.graph.edges.push_back({st.pendingFirstNode, clicked});
                    st.status = "Связь добавлена";
                }
                st.pendingFirstNode = -1;
            }
        }
    }
    ImGui::EndChild();
}

static void DrawOsintTab(AppState& st) {
    ImGui::BeginChild("osint_tab", ImVec2(0, 0), true);
    ImGui::TextWrapped(
        "Инструменты ставятся в локальную папку modules\\ рядом с программой. "
        "Системный Python не затрагивается. Нужен установленный Python 3.9+.");
    ImGui::Separator();

    ImGui::TextUnformatted("Модули:");
    if (ImGui::BeginTable("mod_table", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Инструмент");
        ImGui::TableSetupColumn("Пакет");
        ImGui::TableSetupColumn("Статус");
        ImGui::TableSetupColumn("Действие", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();
        for (auto& m : st.modules->mods) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", m.pipPackage.c_str());
            ImGui::TableSetColumnIndex(2);
            if (m.installed) ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "установлен");
            else             ImGui::TextColored(ImVec4(1,0.5f,0.4f,1), "нет");
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(m.name.c_str());
            if (!m.installed) {
                if (ImGui::Button("Установить", ImVec2(-1, 0)) && !st.osintRunning) {
                    std::string cmd = st.modules->BuildInstallCommand(m);
                    if (cmd.empty()) {
                        st.osintLog += "[ERR] Python не найден в PATH\n";
                        st.osintStatus = "Python не найден";
                    } else {
                        st.osintRunning = true;
                        st.osintStatus  = "Установка " + m.name + "...";
                        st.osintLog    += "$ " + cmd + "\n";
                        std::thread([&st, cmd, name = m.name]() {
                            std::string out = RunCommandAndCapture(cmd);
                            st.osintLog += out + "\n";
                            st.modules->RefreshInstalled();
                            st.osintStatus  = "Установка " + name + " завершена";
                            st.osintRunning = false;
                        }).detach();
                    }
                }
            } else {
                if (ImGui::Button("Удалить", ImVec2(-1, 0))) {
                    std::error_code ec;
                    fs::remove_all(st.modules->root, ec);
                    std::string e;
                    st.modules->EnsureRoot(e);
                    st.modules->RefreshInstalled();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Проверить установку")) st.modules->RefreshInstalled();
    ImGui::Separator();

    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##t", "username или email",
                             st.osintTarget, sizeof(st.osintTarget));
    ImGui::SameLine();
    auto canRun = [&](const ModuleInfo& m) {
        return m.installed && !st.osintRunning && st.osintTarget[0] != '\0';
    };
    for (auto& m : st.modules->mods) {
        ImGui::PushID(m.name.c_str());
        if (ImGui::Button(("Запустить " + m.name).c_str()) && canRun(m)) {
            st.osintRunning = true;
            st.osintStatus  = "Запуск " + m.name + "...";
            std::string exe  = (st.modules->root / "Scripts" / m.exeName).string();
            std::string args = (m.name == "Sherlock") ? " --print-found" : "";
            std::string full = "\"" + exe + "\" " +
                               std::string(st.osintTarget) + args;
            st.osintLog += "$ " + full + "\n";
            std::thread([&st, full, name = m.name]() {
                std::string out = RunCommandAndCapture(full);
                st.osintLog   += out + "\n";
                st.osintStatus = name + " завершён";
                st.osintRunning = false;
            }).detach();
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::NewLine();
    if (st.osintRunning)
        ImGui::TextColored(ImVec4(1,0.8f,0,1), "Выполняется...");
    ImGui::Separator();
    ImGui::Text("Лог:");
    ImGui::BeginChild("log", ImVec2(0, 0), true);
    ImGui::TextUnformatted(st.osintLog.c_str());
    ImGui::EndChild();
    ImGui::EndChild();
}

static void DrawMainScreen(AppState& st) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    DrawTopBar(st);

    ImGui::BeginTabBar("tabs");
    if (ImGui::BeginTabItem("Таблица")) {
        float availH = ImGui::GetContentRegionAvail().y - 30;
        ImGui::BeginChild("body", ImVec2(0, availH), false);
        float tableW = ImGui::GetContentRegionAvail().x - 330;
        ImGui::BeginChild("left", ImVec2(tableW, 0), false);
        DrawTable(st);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, 0), false);
        DrawDetails(st);
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Граф связей")) {
        DrawGraph(st);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("OSINT")) {
        DrawOsintTab(st);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();

    ImGui::Separator();
    ImGui::TextDisabled("%s", st.status.c_str());
    ImGui::End();
}

int main() {
    if (!glfwInit()) { std::cerr << "glfwInit failed\n"; return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* win = glfwCreateWindow(1280, 760, "SearchHelper", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyDarkTheme();

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    ImNodes::CreateContext();
    ImNodes::StyleColorsDark();

    AppState st;
    ModuleManager mm(".");
    st.modules = &mm;

    st.loadProgress = 0.0f;
    st.loadLog = "Запуск...\n";

    int  loadStep = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        if (st.screen == Screen::Loading) {
            auto elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed > 0.35f) {
                if (loadStep == 0) {
                    std::string err;
                    if (EnsureDataFolder("data", err))
                        st.loadLog += "[OK] Папка data готова\n";
                    else
                        st.loadLog += "[ERR] " + err + "\n";

                    if (mm.EnsureRoot(err))
                        st.loadLog += "[OK] Папка modules готова\n";
                    else
                        st.loadLog += "[ERR] " + err + "\n";

                    mm.RefreshInstalled();
                    st.loadProgress = 0.35f;
                    loadStep = 1;
                } else if (loadStep == 1) {
                    std::string err;
                    st.db = LoadDatabase("data", err);
                    st.loadLog += "[OK] Загружено записей: " +
                                  std::to_string(st.db.rows.size()) + "\n";
                    if (!err.empty()) st.loadLog += "[!] " + err;
                    st.loadProgress = 0.75f;
                    loadStep = 2;
                } else if (loadStep == 2) {
                    LoadGraph("data/graph.txt", st.graph);
                    st.loadLog += "[OK] Граф связей: " +
                                  std::to_string(st.graph.edges.size()) + " рёбер\n";
                    st.loadProgress = 1.0f;
                    st.loadLog += "Готово.\n";
                    loadStep = 3;
                } else if (loadStep == 3 && elapsed > 0.9f) {
                    st.screen = Screen::Main;
                    st.status = "Загружено записей: " +
                                std::to_string(st.db.rows.size());
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (st.screen == Screen::Loading) DrawLoadingScreen(st);
        else                             DrawMainScreen(st);

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImNodes::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
