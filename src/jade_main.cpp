#define UNICODE
#define _UNICODE
#define JADE_FRONTEND
#include "JadeView.h"
#include "findimg.cpp"
static uint32_t g_main_window = 0;
static bool g_topmost = false;
static std::atomic<bool> g_jade_ui_ready(false);

static std::string JEsc(const std::wstring& s) { return WideToUtf8(s); }
static const char* JOk(const char* s="ok") { return jade_text_create(s); }
static void NativeBackendInit() {
    g_inst = GetModuleHandleW(nullptr); wchar_t b[MAX_PATH]{}; GetModuleFileNameW(nullptr,b,MAX_PATH); g_base=fs::path(b).parent_path().wstring();
    // 每次运行都是独立会话：不把上一进程的日志混进当前界面。
    { std::lock_guard<std::mutex> lock(g_log_mutex); std::ofstream(fs::path(g_base) / L"findimg.log", std::ios::binary | std::ios::trunc); }
    GdiplusStartupInput gdiIn; GdiplusStartup(&g_gdiplus, &gdiIn, nullptr);
    WNDCLASSW wc{}; wc.hInstance=g_inst; wc.lpfnWndProc=WndProc; wc.lpszClassName=L"FindImgBackend"; wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&wc);
    g_hwnd=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,g_inst,nullptr);
    Add(g_hwnd,L"EDIT",L"0.95",0,ID_THRESHOLD); Add(g_hwnd,L"EDIT",L"1.0",0,ID_INTERVAL); Add(g_hwnd,L"EDIT",L"15",0,ID_COOLDOWN); Add(g_hwnd,L"EDIT",L"60",0,ID_AWAY); Add(g_hwnd,L"EDIT",L"",0,ID_HWND);
    Add(g_hwnd,L"BUTTON",L"",0,ID_CLICK); Add(g_hwnd,L"BUTTON",L"",0,ID_FLASH); Add(g_hwnd,L"BUTTON",L"",0,ID_EMAIL); Add(g_hwnd,L"BUTTON",L"",0,ID_SQUEEZE);
    Add(g_hwnd,L"EDIT",L"",0,ID_FORCE_START); Add(g_hwnd,L"EDIT",L"",0,ID_FORCE_END);
    RefreshGroups(); LoadSettings(); InitJadeConfigSnapshot(); g_startup_complete=true;
    // DM COM 首次激活可能较慢；只在 Jade 主界面已经建立后再执行。
    std::thread([] { while (!g_jade_ui_ready) Sleep(20); Sleep(200); RegisterDmAtStartup(); }).detach();
}
static const char* JADEVIEW_CALL ipc_get(uint32_t,const char*) { std::ostringstream s; s<<"{\"threshold\":\""<<WideToUtf8(ReadCtrl(ID_THRESHOLD))<<"\",\"interval\":\""<<WideToUtf8(ReadCtrl(ID_INTERVAL))<<"\",\"cooldown\":\""<<WideToUtf8(ReadCtrl(ID_COOLDOWN))<<"\",\"away\":\""<<WideToUtf8(ReadCtrl(ID_AWAY))<<"\",\"hwnd\":\""<<WideToUtf8(ReadCtrl(ID_HWND))<<"\",\"force_start\":\""<<WideToUtf8(ReadCtrl(ID_FORCE_START))<<"\",\"force_end\":\""<<WideToUtf8(ReadCtrl(ID_FORCE_END))<<"\",\"click\":"<<(Checked(ID_CLICK)?"true":"false")<<",\"flash\":"<<(Checked(ID_FLASH)?"true":"false")<<",\"email\":"<<(Checked(ID_EMAIL)?"true":"false")<<",\"squeeze\":"<<(Checked(ID_SQUEEZE)?"true":"false")<<",\"running\":"<<(g_running?"true":"false")<<"}"; return jade_text_create(s.str().c_str()); }
static const char* JADEVIEW_CALL ipc_groups(uint32_t,const char*) { std::ostringstream s; s<<"["; bool first=true; std::map<std::wstring,int> counts; for(auto& n:ImageFiles()) if(!IsSqueezeTemplate(n)) counts[GroupKey(n)]++; for(auto& [g,n]:counts){ if(!first)s<<","; first=false; s<<"{\"name\":\""<<WideToUtf8(g)<<"\",\"count\":"<<n<<",\"enabled\":"<<(g_groups[g]?"true":"false")<<"}"; } s<<"]"; return jade_text_create(s.str().c_str()); }
static std::string JEscText(const std::string& x){std::string o;for(char c:x){if(c=='\\'||c=='"')o+='\\',o+=c;else if(c=='\r'){}else if(c=='\n')o+="\\n";else o+=c;}return o;}
static const char* JADEVIEW_CALL ipc_logs(uint32_t,const char*) { std::ifstream f(fs::path(g_base)/L"findimg.log",std::ios::binary); std::vector<std::string> v; std::string line; while(std::getline(f,line)){v.push_back(line);if(v.size()>40)v.erase(v.begin());} std::ostringstream s;s<<"[";for(size_t i=0;i<v.size();++i){if(i)s<<",";s<<"\""<<JEscText(v[i])<<"\"";}s<<"]";return jade_text_create(s.str().c_str()); }
static const char* JADEVIEW_CALL ipc_group_set(uint32_t,const char* p) { std::string x=p?p:"{}"; std::string name=JsonString(x,"group",""); if(name.empty()) name=JsonString(x,"name",""); std::wstring g=Utf8ToWide(name); if(!g.empty()&&g_groups.count(g)){ g_groups[g]=JsonBool(x,"enabled",true); SaveSettings(); Log(std::wstring(L"图片组 ")+g+(g_groups[g]?L" 已启用":L" 已禁用")); } return JOk(); }
static const char* JADEVIEW_CALL ipc_set(uint32_t,const char* p) {
    std::string x=p?p:"{}";
    auto set=[&](int id,const char*k){ if(x.find(std::string("\"")+k+"\"") != std::string::npos) SetJadeText(id, Utf8ToWide(JsonString(x,k,""))); };
    auto setbool=[&](int id,const char*k){ if(x.find(std::string("\"")+k+"\"") != std::string::npos) SetJadeChecked(id, JsonBool(x,k,false)); };
    set(ID_THRESHOLD,"threshold"); set(ID_INTERVAL,"interval"); set(ID_COOLDOWN,"cooldown"); set(ID_AWAY,"away"); set(ID_HWND,"hwnd"); set(ID_FORCE_START,"force_start"); set(ID_FORCE_END,"force_end");
    setbool(ID_CLICK,"click"); setbool(ID_FLASH,"flash"); setbool(ID_EMAIL,"email"); setbool(ID_SQUEEZE,"squeeze");
    g_target = ParseHwnd(ReadCtrl(ID_HWND)); SaveSettings(); return JOk();
}
static const char* JADEVIEW_CALL ipc_pick_window_drag_begin(uint32_t,const char*) {
    return jade_text_create(BeginWindowDragPick() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"already targeting\"}");
}
static const char* JADEVIEW_CALL ipc_pick_window_drag_end(uint32_t,const char*) {
    std::wstring hwnd = FinishWindowDragPick();
    if (hwnd.empty()) return jade_text_create("{\"ok\":false,\"error\":\"cancelled\"}");
    SetJadeText(ID_HWND, hwnd);
    SaveSettings();
    return jade_text_create((std::string("{\"ok\":true,\"hwnd\":\"") + WideToUtf8(hwnd) + "\"}").c_str());
}
static const char* JADEVIEW_CALL ipc_action(uint32_t,const char* p) {
    std::string a=p?p:"";
    if(a.find("pick_window")!=std::string::npos) Log(L"请按住“拖住寻找”并拖到目标窗口后松开");
    else if(a.find("flash_window")!=std::string::npos) FlashAlert();
    else if(a.find("refresh_groups")!=std::string::npos) RefreshGroups(true);
    else if(a.find("open_dir")!=std::string::npos) ShellExecuteW(nullptr,L"open",(fs::path(g_base)/L"img").c_str(),nullptr,g_base.c_str(),SW_SHOWNORMAL);
    else if(a.find("clear_logs")!=std::string::npos) { { std::lock_guard<std::mutex> lock(g_log_mutex); std::ofstream f(fs::path(g_base)/L"findimg.log",std::ios::binary|std::ios::trunc); } Log(L"日志已清空"); }
    else if(a.find("scheme_")!=std::string::npos && a.find("test_scheme_")==std::string::npos) { int n=0; try{n=std::stoi(a.substr(a.find("scheme_")+7));}catch(...){} if(n>=1&&n<=10) SelectScheme(n); else Log(L"截图方案选择无效"); }
    else if(a.find("start")!=std::string::npos) { Log(L"收到启动检测请求"); StartLoop(); }
    else if(a.find("stop")!=std::string::npos) StopLoop(false);
    else if(a.find("test_scheme_")!=std::string::npos){ int n=0; try{n=std::stoi(a.substr(a.find("test_scheme_")+12));}catch(...){} if(n>=1&&n<=10&&!g_testing)std::thread([n]{RunScan(true,n);}).detach(); }
    else if(a.find("test")!=std::string::npos && !g_testing) std::thread([]{RunScan(true);}).detach();
    else if(a.find("keysim")!=std::string::npos) { if(g_key_running) RequestStopKeySimulation(); else StartKeySimulation(); }
    return JOk();
}
static const char* JADEVIEW_CALL ipc_topmost(uint32_t w,const char*) { g_topmost=!g_topmost; set_window_always_on_top(w,g_topmost?1:0); return jade_text_create(g_topmost?"top":"normal"); }
static const char* JADEVIEW_CALL ready(uint32_t,const char*) {
    char u[2048]{};
    fs::path src = fs::path(g_base) / L"runtime" / L"jade_ui" / L"web";
    wchar_t app[MAX_PATH]{}; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", app, MAX_PATH);
    fs::path root = (n && n < MAX_PATH) ? fs::path(app) / L"FindImgJade" / L"web" : src;
    std::error_code ec; fs::create_directories(root, ec);
    fs::copy(src, root, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    std::string root8 = WideToUtf8(root.wstring());
    int ok = set_protocol_service_path(root8.c_str(), u, sizeof(u), 1);
    std::string url;
    if (ok && u[0]) { url = std::string(u); while (!url.empty() && url.back() == '/') url.pop_back(); url += "/"; }
    else { url = "file:///" + root8 + "/"; for (char& c : url) if (c == '\\') c = '/'; }
    AppendDiagnostic(L"JadeView 页面路径：" + root.wstring());
    WebViewWindowOptions o{}; o.title="全屏找图"; o.width=760; o.height=720; o.resizable=1; o.frame_style="borderless";
    o.background_color="#08080eff"; o.theme="dark"; o.minimizable=1; o.maximizable=0; o.x=80; o.y=80; o.use_page_icon=0;
    WebViewSettings s{}; s.focused=1; s.allow_right_click=1;
    uint32_t w = create_webview_window(url.c_str(), 0, &o, &s);
    g_main_window = w; set_window_backdrop(w, "mica");
    HWND hwnd = (HWND)(UINT_PTR)get_window_hwnd(w);
    if (hwnd) { ShowWindow(hwnd, SW_SHOWNORMAL); SetWindowPos(hwnd, HWND_TOP, 80, 80, 760, 720, SWP_SHOWWINDOW); SetForegroundWindow(hwnd); BringWindowToTop(hwnd); }
    g_jade_ui_ready = true;
    return nullptr;
}
static const char* JADEVIEW_CALL closed(uint32_t,const char*) { g_running=false; StopKeySimulation(); std::thread([]{ std::this_thread::sleep_for(std::chrono::milliseconds(300)); ExitProcess(0); }).detach(); return nullptr; }
int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int){ NativeBackendInit(); AppendDiagnostic(L"JadeView 主程序启动"); jade_on(JADEVIEW_EVENT_APP_READY,ready); jade_on(JADEVIEW_EVENT_WINDOW_ALL_CLOSED,closed); register_ipc_handler("win:minimize",[](uint32_t,const char*){ int32_t rc=minimize_window(g_main_window); AppendDiagnostic(L"窗口最小化 IPC 返回："+std::to_wstring(rc)); return JOk();}); register_ipc_handler("win:close",[](uint32_t,const char*){ int32_t rc=close_window(g_main_window); AppendDiagnostic(L"窗口关闭 IPC 返回："+std::to_wstring(rc)); return JOk();}); register_ipc_handler("win:always_on_top",ipc_topmost); register_ipc_handler("find:get",ipc_get); register_ipc_handler("find:logs",ipc_logs); register_ipc_handler("find:groups",ipc_groups); register_ipc_handler("find:group_set",ipc_group_set); register_ipc_handler("find:set",ipc_set); register_ipc_handler("find:pick_window_drag_begin",ipc_pick_window_drag_begin); register_ipc_handler("find:pick_window_drag_end",ipc_pick_window_drag_end); register_ipc_handler("find:action",ipc_action); std::string log=(fs::path(g_base)/L"findimg.log").string(); fs::path data=fs::path(g_base)/L"runtime"/L"jade_data"; std::error_code ec; fs::create_directories(data,ec); std::string dir=WideToUtf8(data.wstring()); if(!JadeView_init(0,log.c_str(),dir.c_str(),"findimg_jade","findimg_jade",1)) { AppendDiagnostic(L"JadeView_init 失败或已有实例运行"); return 1; } return run_message_loop(); }
