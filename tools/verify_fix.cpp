#define UNICODE
#define _UNICODE
#include "findimg.cpp"
#include <cstdio>

// 端到端验证：完整复刻 NativeBackendInit + RunScan 的路径
//   1) GdiplusStartup（本次修复点）
//   2) RefreshGroups：按 img/ 文件注册分组
//   3) LoadSettings：用修复后的正则读取 group_selection
//   4) Templates()：扫描时实际拿到的模板集合
int main(int argc, char** argv) {
    // 项目根目录：默认取当前工作目录，也可用第一个参数显式指定。
    if (argc > 1) g_base = Utf8ToWide(argv[1]);
    else g_base = fs::current_path().wstring();

    GdiplusStartupInput in;
    Status st = GdiplusStartup(&g_gdiplus, &in, nullptr);
    printf("[1] GdiplusStartup status=%d (0=Ok)\n", (int)st);

    auto files = ImageFiles();
    printf("[2] ImageFiles() = %zu 个文件\n", files.size());

    for (auto& n : files) if (!IsSqueezeTemplate(n)) g_groups[GroupKey(n)] = true;
    printf("[3] RefreshGroups 后启用组 = %zu\n", g_groups.size());

    // 复刻 LoadSettings 的解析（使用修复后的 \\s* 正则）
    std::string text = ReadText(fs::path(g_base) / SETTINGS_FILE);
    for (auto& [k, v] : g_groups) v = true;
    auto pos = text.find("\"group_selection\"");
    if (pos != std::string::npos) {
        auto end = text.find('}', pos); if (end == std::string::npos) end = text.size();
        std::string part = text.substr(pos, end - pos);
        std::regex r("\"([^\"]+)\"\\s*:\\s*(true|false)");
        int applied = 0;
        for (std::sregex_iterator i(part.begin(), part.end(), r), e; i != e; ++i) {
            std::wstring k = Utf8ToWide((*i)[1].str());
            if (g_groups.count(k)) { g_groups[k] = (*i)[2].str() == "true"; ++applied; }
        }
        printf("[4] LoadSettings 正则匹配并应用 %d 条组开关\n", applied);
    }

    int enabled = 0;
    for (auto& [k, v] : g_groups) if (v) ++enabled;
    printf("[5] 最终启用组数 = %d\n", enabled);

    auto ts = Templates();
    printf("[6] Templates() 返回 %zu 个可用模板\n", ts.size());

    std::ofstream out(fs::path(g_base) / L"docs" / L"verify_result.txt", std::ios::binary);
    out << "=== 真实运行时验证（读取实际 settings + img/）===\n\n";
    out << "分组开关（来自 findimg_settings.json）：\n";
    for (auto& [k, v] : g_groups) out << "    " << (v ? "[x] " : "[ ] ") << WideToUtf8(k) << "\n";
    out << "\nTemplates() 实际返回 " << ts.size() << " 个模板：\n";
    for (auto& t : ts) out << "    [" << WideToUtf8(t.group) << "] " << WideToUtf8(t.name)
        << " (" << t.w << "x" << t.h << ", valid=" << t.valid.size() << ")\n";
    if (ts.empty()) out << "    >>> 失败：没有可读模板\n";
    else out << "\n>>> 成功：扫描将使用以上 " << ts.size() << " 个模板\n";
    out.close();
    printf("[7] 明细写入 verify_result.txt\n");

    GdiplusShutdown(g_gdiplus);
    return 0;
}
