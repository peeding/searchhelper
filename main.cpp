#include <windows.h>
#include <windowsx.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <sstream>

namespace fs = std::filesystem;

constexpr int ID_QUERY = 101;
constexpr int ID_GO = 102;
constexpr int ID_TABLE = 103;
constexpr int ID_GRAPH = 104;
constexpr int ID_LIST = 105;
constexpr int ID_CANVAS = 106;
constexpr int ID_STATUS = 107;
constexpr int ID_DETAILS = 108;
HBRUSH g_bgBrush=nullptr, g_panelBrush=nullptr, g_editBrush=nullptr;

struct Record {
    std::wstring database;
    int csvLine = 0;
    std::vector<std::pair<std::wstring,std::wstring>> fields;
    std::wstring summary;
};
struct GraphNode {
    enum Kind { RecordNode, ValueNode } kind = RecordNode;
    std::wstring title, subtitle, key;
    int x=0, y=0, w=210, h=64;
    int recordIndex=-1;
    int connections=0;
};
struct GraphEdge { int from=0, to=0; };
struct DatabaseCount { std::wstring name; int count=0; };

HWND g_main=nullptr, g_query=nullptr, g_go=nullptr, g_table=nullptr, g_graph=nullptr;
HWND g_list=nullptr, g_canvas=nullptr, g_status=nullptr, g_hint=nullptr, g_details=nullptr;
HFONT g_font=nullptr, g_fontBold=nullptr, g_fontSmall=nullptr;
bool g_graphMode=false;
std::vector<Record> g_records;
std::vector<DatabaseCount> g_counts;
std::vector<GraphNode> g_nodes;
std::vector<GraphEdge> g_edges;
int g_dragNode=-1, g_dragDx=0, g_dragDy=0;
int g_totalFiles=0;

COLORREF C_BG=RGB(14,19,30), C_PANEL=RGB(23,31,47), C_PANEL2=RGB(30,41,60);
COLORREF C_TEXT=RGB(231,238,248), C_MUTED=RGB(150,165,186), C_ACCENT=RGB(75,145,255);
COLORREF C_LINE=RGB(72,94,126), C_RECORD=RGB(37,87,151), C_VALUE=RGB(35,104,91);

std::wstring lower(std::wstring s) {
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
    return s;
}
std::wstring trim(std::wstring s) {
    size_t a=s.find_first_not_of(L" \t\r\n");
    if(a==s.npos) return {};
    return s.substr(a,s.find_last_not_of(L" \t\r\n")-a+1);
}
std::wstring decodeUtf8(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    UINT cp=CP_UTF8;
    if(n<=0) { cp=CP_ACP; n=MultiByteToWideChar(cp,0,s.data(),(int)s.size(),nullptr,0); }
    if(n<=0) return {};
    std::wstring out(n,L'\0');
    MultiByteToWideChar(cp,0,s.data(),(int)s.size(),out.data(),n);
    if(!out.empty() && out.front()==0xFEFF) out.erase(out.begin());
    return out;
}
wchar_t detectDelimiter(const std::wstring& s) {
    wchar_t choices[]={L',',L';',L'\t'}, best=L','; int maxCount=-1;
    for(wchar_t d:choices) {
        int n=0; bool quoted=false;
        for(size_t i=0;i<s.size();++i) {
            if(s[i]==L'"') {
                if(quoted && i+1<s.size() && s[i+1]==L'"') { ++i; continue; }
                quoted=!quoted;
            } else if(s[i]==d && !quoted) ++n;
        }
        if(n>maxCount) { maxCount=n; best=d; }
    }
    return best;
}
std::vector<std::wstring> parseCsv(const std::wstring& s,wchar_t d) {
    std::vector<std::wstring> out; std::wstring field; bool quoted=false;
    for(size_t i=0;i<s.size();++i) {
        wchar_t c=s[i];
        if(c==L'"') {
            if(quoted && i+1<s.size() && s[i+1]==L'"') { field+=L'"'; ++i; }
            else quoted=!quoted;
        } else if(c==d && !quoted) { out.push_back(field); field.clear(); }
        else field+=c;
    }
    out.push_back(field); return out;
}
fs::path appFolder() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr,p,MAX_PATH);
    return fs::path(p).parent_path();
}
void setStatus(const std::wstring& s) { SetWindowTextW(g_status,s.c_str()); }

void setFont(HWND h, HFONT f=g_font) { SendMessageW(h,WM_SETFONT,(WPARAM)f,TRUE); }
HWND makeLabel(HWND parent,int id,const wchar_t* text,int x,int y,int w,int h,HFONT f=nullptr) {
    HWND c=CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE|SS_LEFT, x,y,w,h,parent,(HMENU)(INT_PTR)id,nullptr,nullptr);
    setFont(c,f?f:g_font); return c;
}
HWND makeButton(HWND parent,int id,const wchar_t* text) {
    HWND c=CreateWindowW(L"BUTTON",text,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|WS_TABSTOP,0,0,100,34,parent,(HMENU)(INT_PTR)id,nullptr,nullptr);
    setFont(c); return c;
}
void addListColumn(int i,const wchar_t* name,int width) {
    LVCOLUMNW c{}; c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
    c.pszText=(LPWSTR)name; c.cx=width; c.iSubItem=i;
    ListView_InsertColumn(g_list,i,&c);
}

void showRecordDetails(int index) {
    if(!g_details) return;
    if(index<0 || index>=(int)g_records.size()) { SetWindowTextW(g_details,L"Выберите строку, чтобы увидеть все найденные поля."); return; }
    const auto& r=g_records[index];
    std::wstring text=L"БАЗА: "+r.database+L"    |    СТРОКА CSV: "+std::to_wstring(r.csvLine)+L"\r\n\r\n";
    for(const auto& f:r.fields) { text+=f.first+L": "; text+=f.second.empty()?L"—":f.second; text+=L"\r\n"; }
    SetWindowTextW(g_details,text.c_str());
}
void drawButton(const DRAWITEMSTRUCT* d) {
    bool hot=(d->itemState&ODS_HOTLIGHT)!=0, down=(d->itemState&ODS_SELECTED)!=0;
    COLORREF fill=down?RGB(39,91,160):(hot?RGB(48,76,116):RGB(34,48,70));
    if((int)d->CtlID==ID_GO) fill=down?RGB(35,103,190):(hot?RGB(74,143,240):C_ACCENT);
    HBRUSH b=CreateSolidBrush(fill); HPEN p=CreatePen(PS_SOLID,1,fill);
    HGDIOBJ ob=SelectObject(d->hDC,b),op=SelectObject(d->hDC,p);
    RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,9,9);
    SelectObject(d->hDC,ob);SelectObject(d->hDC,op);DeleteObject(b);DeleteObject(p);
    SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,C_TEXT);
    wchar_t label[128]{};GetWindowTextW(d->hwndItem,label,128);
    RECT r=d->rcItem;DrawTextW(d->hDC,label,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}

void fillTable() {
    ListView_DeleteAllItems(g_list);
    while(Header_GetItemCount(ListView_GetHeader(g_list))>0) ListView_DeleteColumn(g_list,0);
    addListColumn(0,L"База данных",170);
    addListColumn(1,L"Строка",75);
    int available=900; RECT rc{}; GetClientRect(g_list,&rc); available=(std::max)(520,static_cast<int>(rc.right)-270);
    int longest=available; HDC dc=GetDC(g_list); SelectObject(dc,g_font);
    for(const auto& rec:g_records) { SIZE z{}; GetTextExtentPoint32W(dc,rec.summary.c_str(),(int)rec.summary.size(),&z); longest=(std::max)(longest,(int)z.cx+36); }
    ReleaseDC(g_list,dc); addListColumn(2,L"Найденные данные",(std::max)(available,longest));
    for(int i=0;i<(int)g_records.size();++i) {
        const auto& r=g_records[i];
        LVITEMW item{}; item.mask=LVIF_TEXT; item.iItem=i; item.pszText=(LPWSTR)r.database.c_str();
        ListView_InsertItem(g_list,&item);
        std::wstring ln=std::to_wstring(r.csvLine);
        ListView_SetItemText(g_list,i,1,(LPWSTR)ln.c_str());
        ListView_SetItemText(g_list,i,2,(LPWSTR)r.summary.c_str());
    }
    if(!g_records.empty()) { ListView_SetItemState(g_list,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); showRecordDetails(0); }
    else showRecordDetails(-1);
}

void buildGraph() {
    g_nodes.clear(); g_edges.clear();
    std::unordered_map<std::wstring,int> values;
    for(int ri=0;ri<(int)g_records.size() && ri<180;++ri) {
        const auto& rec=g_records[ri];
        GraphNode person; person.kind=GraphNode::RecordNode;
        person.title=rec.fields.empty()?L"Запись":trim(rec.fields[0].second);
        if(person.title.empty()) person.title=L"Запись "+std::to_wstring(ri+1);
        if(person.title.size()>30) person.title=person.title.substr(0,27)+L"...";
        person.subtitle=rec.database+L" • строка "+std::to_wstring(rec.csvLine);
        person.recordIndex=ri;
        int p=(int)g_nodes.size(); g_nodes.push_back(person);

        for(const auto& field:rec.fields) {
            std::wstring val=trim(field.second), name=trim(field.first);
            if(val.empty() || val.size()>140) continue;
            std::wstring key=lower(name)+L"\x1f"+lower(val);
            int v;
            auto it=values.find(key);
            if(it==values.end()) {
                GraphNode node; node.kind=GraphNode::ValueNode;
                node.title=val.size()>34?val.substr(0,31)+L"...":val;
                node.subtitle=name.empty()?L"Поле CSV":name;
                node.key=key;
                v=(int)g_nodes.size(); g_nodes.push_back(std::move(node)); values.emplace(key,v);
            } else v=it->second;
            if(v!=p) { g_edges.push_back({p,v}); g_nodes[v].connections++; }
        }
    }
    // Lay out two columns; shared values become junctions between records.
    int records=0, vals=0;
    for(auto& n:g_nodes) {
        if(n.kind==GraphNode::RecordNode) ++records; else ++vals;
    }
    int ri=0,vi=0;
    for(auto& n:g_nodes) {
        if(n.kind==GraphNode::RecordNode) { n.x=34; n.y=30+ri*104; n.w=245; ++ri; }
        else { n.x=430; n.y=25+vi*82; n.w=285; ++vi; }
    }
}
void drawRoundRect(HDC dc,RECT r,int radius,COLORREF fill,COLORREF border) {
    HBRUSH b=CreateSolidBrush(fill); HPEN p=CreatePen(PS_SOLID,1,border);
    HGDIOBJ ob=SelectObject(dc,b), op=SelectObject(dc,p);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);
    SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(p);
}
void drawGraph(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps);
    RECT client{}; GetClientRect(hwnd,&client);
    HBRUSH bg=CreateSolidBrush(C_BG); FillRect(dc,&client,bg); DeleteObject(bg);
    SetBkMode(dc,TRANSPARENT);
    // Subtle grid.
    HPEN grid=CreatePen(PS_SOLID,1,RGB(27,37,54)); HGDIOBJ old=SelectObject(dc,grid);
    for(int x=0;x<client.right;x+=32){MoveToEx(dc,x,0,nullptr);LineTo(dc,x,client.bottom);}
    for(int y=0;y<client.bottom;y+=32){MoveToEx(dc,0,y,nullptr);LineTo(dc,client.right,y);}
    SelectObject(dc,old); DeleteObject(grid);
    // Connections first.
    HPEN edge=CreatePen(PS_SOLID,2,C_LINE); old=SelectObject(dc,edge);
    for(const auto& e:g_edges) {
        if(e.from>=(int)g_nodes.size()||e.to>=(int)g_nodes.size()) continue;
        const auto&a=g_nodes[e.from]; const auto&b=g_nodes[e.to];
        MoveToEx(dc,a.x+a.w,a.y+a.h/2,nullptr);
        POINT bez[3]={{a.x+a.w+55,a.y+a.h/2},{b.x-55,b.y+b.h/2},{b.x,b.y+b.h/2}};
        PolyBezierTo(dc,bez,3);
    }
    SelectObject(dc,old); DeleteObject(edge);
    for(const auto& n:g_nodes) {
        RECT r{n.x,n.y,n.x+n.w,n.y+n.h};
        COLORREF fill=n.kind==GraphNode::RecordNode?C_RECORD:C_VALUE;
        drawRoundRect(dc,r,12,fill,n.kind==GraphNode::RecordNode?RGB(82,153,245):RGB(68,170,145));
        SetTextColor(dc,C_TEXT);
        RECT title{r.left+12,r.top+9,r.right-10,r.top+32};
        DrawTextW(dc,n.title.c_str(),-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SetTextColor(dc,RGB(197,211,231));
        RECT sub{r.left+12,r.top+35,r.right-10,r.bottom-6};
        std::wstring subtitle=n.subtitle;
        if(n.kind==GraphNode::ValueNode && n.connections>1)
            subtitle+=L"  •  связей: "+std::to_wstring(n.connections);
        DrawTextW(dc,subtitle.c_str(),-1,&sub,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    if(g_nodes.empty()) {
        SetTextColor(dc,C_MUTED);
        RECT r{30,30,client.right-30,90};
        DrawTextW(dc,L"После поиска здесь появится граф найденных записей и общих значений.",-1,&r,DT_LEFT|DT_TOP|DT_WORDBREAK);
    }
    EndPaint(hwnd,&ps);
}
int hitNode(int x,int y) {
    for(int i=(int)g_nodes.size()-1;i>=0;--i) {
        const auto& n=g_nodes[i];
        if(x>=n.x&&x<=n.x+n.w&&y>=n.y&&y<=n.y+n.h)return i;
    }
    return -1;
}
LRESULT CALLBACK canvasProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: drawGraph(hwnd); return 0;
    case WM_LBUTTONDOWN: {
        int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp); g_dragNode=hitNode(x,y);
        if(g_dragNode>=0) { SetCapture(hwnd); g_dragDx=x-g_nodes[g_dragNode].x; g_dragDy=y-g_nodes[g_dragNode].y; }
        return 0;
    }
    case WM_MOUSEMOVE:
        if(g_dragNode>=0 && (wp&MK_LBUTTON)) {
            g_nodes[g_dragNode].x=GET_X_LPARAM(lp)-g_dragDx;
            g_nodes[g_dragNode].y=GET_Y_LPARAM(lp)-g_dragDy;
            InvalidateRect(hwnd,nullptr,FALSE);
        } return 0;
    case WM_LBUTTONUP:
        if(g_dragNode>=0) { ReleaseCapture(); g_dragNode=-1; } return 0;
    case WM_LBUTTONDBLCLK: return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void switchView(bool graph) {
    g_graphMode=graph;
    ShowWindow(g_list,graph?SW_HIDE:SW_SHOW);
    ShowWindow(g_canvas,graph?SW_SHOW:SW_HIDE);
    InvalidateRect(g_canvas,nullptr,TRUE);
    InvalidateRect(g_main,nullptr,TRUE);
}
void doSearch() {
    wchar_t buf[1024]{}; GetWindowTextW(g_query,buf,1024);
    std::wstring query=trim(buf);
    g_records.clear(); g_counts.clear(); g_totalFiles=0;
    if(query.empty()) { setStatus(L"Введите запрос для поиска."); fillTable(); buildGraph(); InvalidateRect(g_canvas,nullptr,TRUE); return; }
    fs::path folder=appFolder()/L"data"; std::error_code ec; fs::create_directories(folder,ec);
    for(const auto& entry:fs::directory_iterator(folder,ec)) {
        if(ec || !entry.is_regular_file() || lower(entry.path().extension().wstring())!=L".csv") continue;
        std::ifstream f(entry.path(),std::ios::binary); if(!f) continue;
        std::string line; std::vector<std::wstring> lines;
        while(std::getline(f,line)) { if(!line.empty()&&line.back()=='\r')line.pop_back(); lines.push_back(decodeUtf8(line)); }
        if(lines.size()<2) continue;
        ++g_totalFiles;
        auto headers=parseCsv(lines[0],detectDelimiter(lines[0]));
        wchar_t delim=detectDelimiter(lines[0]);
        int before=(int)g_records.size();
        for(size_t li=1;li<lines.size();++li) {
            auto cells=parseCsv(lines[li],delim);
            bool match=false;
            for(const auto& cell:cells) if(lower(cell).find(lower(query))!=std::wstring::npos){match=true;break;}
            if(!match)continue;
            Record r; r.database=entry.path().stem().wstring(); r.csvLine=(int)li+1;
            for(size_t ci=0;ci<cells.size();++ci) {
                std::wstring h=ci<headers.size()?trim(headers[ci]):L"Колонка "+std::to_wstring(ci+1);
                r.fields.push_back({h,cells[ci]});
                if(!trim(cells[ci]).empty()) { if(!r.summary.empty())r.summary+=L"  |  "; r.summary+=h+L": "+trim(cells[ci]); }
            }
            if(r.summary.size()>1800)r.summary.resize(1800);
            g_records.push_back(std::move(r));
            if(g_records.size()>=180)break;
        }
        int added=(int)g_records.size()-before;
        if(added>0)g_counts.push_back({entry.path().stem().wstring(),added});
        if(g_records.size()>=180)break;
    }
    fillTable(); buildGraph(); InvalidateRect(g_canvas,nullptr,TRUE);
    if(g_records.empty()) setStatus(L"Совпадений не найдено. Проверено CSV-файлов: "+std::to_wstring(g_totalFiles));
    else setStatus(L"Найдено записей: "+std::to_wstring(g_records.size())+L"  •  Баз с совпадениями: "+std::to_wstring(g_counts.size())+L"  •  CSV проверено: "+std::to_wstring(g_totalFiles)+(g_records.size()>=180?L"  •  Показаны первые 180":L""));
}
void layout(HWND w) {
    RECT r{}; GetClientRect(w,&r); int W=r.right,H=r.bottom,M=26;
    MoveWindow(GetDlgItem(w,201),M,18,W-2*M,42,TRUE);
    MoveWindow(GetDlgItem(w,202),M,61,W-2*M,25,TRUE);
    MoveWindow(g_query,M,105,W-2*M-142,42,TRUE);
    MoveWindow(g_go,W-M-128,105,128,42,TRUE);
    MoveWindow(g_table,M,164,124,36,TRUE);
    MoveWindow(g_graph,M+134,164,136,36,TRUE);
    MoveWindow(g_hint,M+290,170,W-M-310,24,TRUE);
    int contentTop=210, statusH=30, detailsH=(std::max)(130,H/5);
    int listH=(std::max)(170,H-contentTop-statusH-detailsH-18);
    MoveWindow(g_list,M,contentTop,W-2*M,listH,TRUE);
    MoveWindow(g_canvas,M,contentTop,W-2*M,listH,TRUE);
    MoveWindow(g_details,M,contentTop+listH+10,W-2*M,detailsH,TRUE);
    MoveWindow(g_status,0,H-statusH,W,statusH,TRUE);
}
LRESULT CALLBACK mainProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_ERASEBKGND: { RECT r{};GetClientRect(w,&r);FillRect((HDC)wp,&r,g_bgBrush?g_bgBrush:(HBRUSH)(COLOR_WINDOW+1));return 1; }
    case WM_CTLCOLORSTATIC: { HDC dc=(HDC)wp;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,C_TEXT);return (LRESULT)(g_bgBrush?g_bgBrush:GetStockObject(BLACK_BRUSH)); }
    case WM_CTLCOLOREDIT: { HDC dc=(HDC)wp;SetTextColor(dc,C_TEXT);SetBkColor(dc,RGB(25,35,52));return (LRESULT)(g_editBrush?g_editBrush:GetStockObject(BLACK_BRUSH)); }
    case WM_DRAWITEM: drawButton((DRAWITEMSTRUCT*)lp);return TRUE;
    case WM_NOTIFY: {
        auto* hdr=(NMHDR*)lp;
        if(hdr->hwndFrom==g_list && hdr->code==LVN_ITEMCHANGED) {
            auto* n=(NMLISTVIEW*)lp;
            if((n->uNewState&LVIS_SELECTED) && n->iItem>=0) showRecordDetails(n->iItem);
        }
        if(hdr->hwndFrom==g_list && hdr->code==NM_CUSTOMDRAW) {
            auto* cd=(NMLVCUSTOMDRAW*)lp;
            if(cd->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
            if(cd->nmcd.dwDrawStage==CDDS_ITEMPREPAINT){cd->clrText=C_TEXT;cd->clrTextBk=(cd->nmcd.dwItemSpec%2)?C_PANEL:C_PANEL2;return CDRF_NOTIFYSUBITEMDRAW;}
            if(cd->nmcd.dwDrawStage==(CDDS_ITEMPREPAINT|CDDS_SUBITEM)){cd->clrText=C_TEXT;cd->clrTextBk=(cd->nmcd.dwItemSpec%2)?C_PANEL:C_PANEL2;return CDRF_NEWFONT;}
        }
        break;
    }
    case WM_CREATE: {
        g_font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        g_fontBold=CreateFontW(-30,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        g_fontSmall=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        makeLabel(w,201,L"SearchHelper",0,0,0,0,g_fontBold);
        makeLabel(w,202,L"Локальный поиск по вашим CSV-файлам  ·  таблица и граф связей",0,0,0,0,g_font);
        g_query=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|WS_TABSTOP,0,0,100,40,w,(HMENU)(INT_PTR)ID_QUERY,nullptr,nullptr);setFont(g_query);
        g_go=CreateWindowW(L"BUTTON",L"Найти",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,0,0,100,40,w,(HMENU)(INT_PTR)ID_GO,nullptr,nullptr);
        g_table=CreateWindowW(L"BUTTON",L"Таблица",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,0,0,100,36,w,(HMENU)(INT_PTR)ID_TABLE,nullptr,nullptr);
        g_graph=CreateWindowW(L"BUTTON",L"Граф связей",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,0,0,100,36,w,(HMENU)(INT_PTR)ID_GRAPH,nullptr,nullptr);
        setFont(g_go);setFont(g_table);setFont(g_graph);
        g_hint=makeLabel(w,203,L"Перетащите узлы мышью. Общие значения объединяются в один узел.",0,0,0,0,g_fontSmall);
        g_list=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL,0,0,100,100,w,(HMENU)(INT_PTR)ID_LIST,nullptr,nullptr);
        ListView_SetExtendedListViewStyle(g_list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_HEADERDRAGDROP);setFont(g_list);
        ListView_SetBkColor(g_list,C_PANEL);ListView_SetTextBkColor(g_list,C_PANEL);ListView_SetTextColor(g_list,C_TEXT);
        SetWindowTheme(g_list,L"DarkMode_Explorer",nullptr);
        SetWindowTheme(ListView_GetHeader(g_list),L"DarkMode_Explorer",nullptr);
        g_details=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Выберите строку, чтобы увидеть все найденные поля.",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,0,0,100,100,w,(HMENU)(INT_PTR)ID_DETAILS,nullptr,nullptr);
        setFont(g_details);SetWindowTheme(g_details,L"DarkMode_Explorer",nullptr);
        g_canvas=CreateWindowExW(WS_EX_CLIENTEDGE,L"SearchHelperGraphCanvas",L"",WS_CHILD|WS_CLIPCHILDREN,0,0,100,100,w,(HMENU)(INT_PTR)ID_CANVAS,nullptr,nullptr);
        g_status=CreateWindowW(L"STATIC",L"  Готово. CSV-файлы поместите в папку data рядом с программой.",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,100,28,w,(HMENU)(INT_PTR)ID_STATUS,nullptr,nullptr);
        setFont(g_status);
        g_bgBrush=CreateSolidBrush(C_BG);g_panelBrush=CreateSolidBrush(C_PANEL);g_editBrush=CreateSolidBrush(RGB(25,35,52));
        SetWindowTheme(g_status,L"DarkMode_Explorer",nullptr);
        fs::create_directories(appFolder()/L"data");
        layout(w); fillTable(); switchView(false);
        return 0;
    }
    case WM_SIZE: layout(w); return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==ID_GO){doSearch();return 0;}
        if(LOWORD(wp)==ID_TABLE){switchView(false);return 0;}
        if(LOWORD(wp)==ID_GRAPH){switchView(true);return 0;}
        break;
    case WM_GETMINMAXINFO:{auto*m=(MINMAXINFO*)lp;m->ptMinTrackSize.x=820;m->ptMinTrackSize.y=560;return 0;}
    case WM_DESTROY:
        if(g_font)DeleteObject(g_font);if(g_fontBold)DeleteObject(g_fontBold);if(g_fontSmall)DeleteObject(g_fontSmall);
        if(g_bgBrush)DeleteObject(g_bgBrush);if(g_panelBrush)DeleteObject(g_panelBrush);if(g_editBrush)DeleteObject(g_editBrush);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,PWSTR,int show) {
    INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};InitCommonControlsEx(&cc);
    WNDCLASSW canvas{};canvas.lpfnWndProc=canvasProc;canvas.hInstance=inst;canvas.lpszClassName=L"SearchHelperGraphCanvas";canvas.hCursor=LoadCursorW(nullptr,IDC_ARROW);canvas.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);RegisterClassW(&canvas);
    WNDCLASSW wc{};wc.lpfnWndProc=mainProc;wc.hInstance=inst;wc.lpszClassName=L"SearchHelperMain";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    if(!RegisterClassW(&wc))return 1;
    HWND w=CreateWindowW(wc.lpszClassName,L"SearchHelper",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1180,820,nullptr,nullptr,inst,nullptr);
    if(!w)return 1;
    ShowWindow(w,show);UpdateWindow(w);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return (int)msg.wParam;
}
