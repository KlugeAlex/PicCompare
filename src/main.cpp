#include "image.h"
#include "interaction.h"

#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace pic;
using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT workReady = WM_APP + 1;
constexpr UINT_PTR reorderScrollTimer = 3;
constexpr wchar_t windowClass[] = L"PicCompare.Desktop";
constexpr wchar_t preferences[] = L"Software\\PicCompare";
constexpr double toolbar = 64, header = 60, footer = 54, gutter = 46, rowHeight = 56;

struct Palette {
    D2D1_COLOR_F bg{D2D1::ColorF(0x131923)}, panel{D2D1::ColorF(0x1b2432)},
        surface{D2D1::ColorF(0x222d3e)}, line{D2D1::ColorF(0x354158)},
        fg{D2D1::ColorF(0xf0f4fb)}, muted{D2D1::ColorF(0xa7b6cc)},
        blue{D2D1::ColorF(0x8ab9ff)}, primary{D2D1::ColorF(0x2b66c1)},
        selected{D2D1::ColorF(0x253b59)}, canvas{D2D1::ColorF(0x15171a)},
        danger{D2D1::ColorF(0x9c3344)}, pink{D2D1::ColorF(0xffa5af)},
        amber{D2D1::ColorF(0xffcc76)};
} const color;

D2D1_RECT_F rect(Box b) {
    return D2D1::RectF(float(b.l), float(b.t), float(b.r), float(b.b));
}
D2D1_POINT_2F point(Point p) { return D2D1::Point2F(float(p.x), float(p.y)); }
Box inset(Box b, double n) { return {b.l + n, b.t + n, b.r - n, b.b - n}; }
double fit(Box b, double w, double h) { return (std::min)(b.width() / w, b.height() / h); }

std::wstring fixed(double value, int precision = 1) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.*f", precision, value);
    return buffer;
}
std::wstring displayPath(const fs::path& path) {
    const auto parent = path.parent_path();
    auto name = parent.filename().wstring();
    if (name.empty()) name = parent.root_name().wstring();
    if (name.empty()) return path.filename().wstring();
    return name + L" / " + path.filename().wstring();
}

std::wstring bytes(uintmax_t count) {
    if (count < 1024) return std::to_wstring(count) + L" B";
    if (count < 1024 * 1024) return fixed(double(count) / 1024) + L" KB";
    if (count < 1024ull * 1024 * 1024) return fixed(double(count) / (1024 * 1024)) + L" MB";
    return fixed(double(count) / (1024ull * 1024 * 1024)) + L" GB";
}
bool held(int key) { return (GetKeyState(key) & 0x8000) != 0; }

struct Worker {
    using Job = std::function<void(IWICImagingFactory*)>;
    std::mutex lock;
    std::condition_variable wake;
    bool done{};
    std::deque<Job> foreground, background;
    std::thread thread;

    Worker() : thread([this] {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        for (;;) {
            Job job;
            {
                std::unique_lock guard(lock);
                wake.wait(guard, [this] { return done || !foreground.empty() || !background.empty(); });
                if (done) break;
                auto& queue = foreground.empty() ? background : foreground;
                job = std::move(queue.front());
                queue.pop_front();
            }
            job(wic.Get());
        }
        wic.Reset();
        if (SUCCEEDED(apartment)) CoUninitialize();
    }) {}

    void push(Job job, bool priority = false, bool replace = false) {
        {
            std::lock_guard guard(lock);
            auto& queue = priority ? foreground : background;
            if (replace) queue.clear();
            queue.emplace_back(std::move(job));
        }
        wake.notify_one();
    }

    ~Worker() {
        {
            std::lock_guard guard(lock);
            done = true;
        }
        wake.notify_one();
        thread.join();
    }
};

struct Entry {
    fs::path path;
    uintmax_t size{};
    fs::file_time_type time{};
    std::shared_ptr<Raster> thumb;
    bool requested{}, broken{};
};
struct Column {
    fs::path path;
    std::vector<Entry> files;
    std::set<fs::path> hidden;
    std::wstring error;
    bool manualOrder{};
};
struct TreeNode {
    fs::path path;
    std::wstring label;
    std::vector<TreeNode> children;
    bool expanded{}, scanned{};
};
struct Slot {
    size_t column{}, row{};
    fs::path path;
    std::shared_ptr<Image> image;
    Orientation orientation;
    Point offset; // Independent pan in oriented image pixels.
    std::optional<Stats> measurement;
};
struct Pane {
    size_t slot{};
    Box area;
};
struct View {
    Box area;
    Point top;
    Point size;
    double scale{};
    Point normal(Point p) const {
        return {(p.x - top.x) / (scale * size.x), (p.y - top.y) / (scale * size.y)};
    }
    Point screen(Point p) const { return {top.x + p.x * size.x * scale, top.y + p.y * size.y * scale}; }
};

enum class Command {
    Add, SortName, SortTime, Order, Compare, Back, Previous, Next, Split, Help, ToggleTree,
    Refresh, ColumnSelect, ColumnClose, ColumnPath, RowSelect, FileSelect, TreeOpen, TreeToggle,
    TreeAdd, Remove, ConfirmDelete, Cancel, Range, RangePrev, RangeNext, RangeOpen, CopyPath,
    Hist, Probe, Reset, Actual, Info, SplitMore, SplitFewer
};
struct Hit {
    Box box;
    Command action;
    size_t a{}, b{};
    std::wstring tip;
};
enum class Modal { None, Help, Delete, Permanent, TooMany, Range };
enum class Gesture { None, Group, Single, NewRoi, EditRoi, TreeDrag, ColumnDrag, FileDrag };
struct BrowserGrid {
    Box area;
    double bodyTop{}, columnWidth{};
};
struct ReorderDrop {
    size_t boundary{};
    Box indicator;
};
struct Surface {
    HWND hwnd{};
    bool secondary{};
    double dpi{1};
    double width{}, height{};
    Point mouse{-1, -1};
    std::vector<Hit> hits;
    std::vector<Pane> panes;
    BrowserGrid grid;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    struct Cached { std::weak_ptr<Raster> source; ComPtr<ID2D1Bitmap> bitmap; };
    std::unordered_map<const Raster*, std::vector<Cached>> bitmaps;
};
struct Drag {
    Gesture mode{};
    HWND hwnd{};
    size_t slot{};
    unsigned edges{};
    bool singleRoi{};
    Point start, previous;
    fs::path folder;
    std::optional<Hit> reorder;
    bool ctrl{}, moved{};
};

class App {
public:
    explicit App(HINSTANCE instance, bool persist) : instance_(instance), persist_(persist) {}
    bool create(const std::vector<fs::path>& folders);
    LRESULT message(Surface&, HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);

private:
    HINSTANCE instance_;
    bool persist_;
    Surface main_, second_;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> write_;
    std::map<int, ComPtr<IDWriteTextFormat>> fonts_;
    std::vector<Column> columns_;
    std::vector<TreeNode> tree_;
    std::vector<fs::path> treePaths_;
    fs::path currentFolder_;
    std::set<fs::path> selection_;
    std::vector<std::pair<size_t, size_t>> pendingSelection_;
    std::vector<fs::path> pendingDelete_;
    std::vector<Slot> slots_;
    std::optional<Roi> roi_;
    std::optional<Roi> roiPreview_;
    std::optional<Point> probe_;
    size_t hotSlot_{}, splitBoundary_{};
    Drag drag_;
    std::optional<WINDOWPLACEMENT> mainBeforeSplit_;
    bool comparing_{}, split_{}, byTime_{}, descending_{}, treeVisible_{true};
    bool histogram_{true}, info_{true}, probeEnabled_{true}, cHeld_{}, sHeld_{}, diffOn_{};
    bool actual_{};
    double zoom_{1}, gain_{1}, scrollY_{}, scrollX_{}, treeY_{};
    Point groupPan_, zoomShift_;
    size_t generation_{}, measurementGeneration_{}, rangeStart_{}, treeRows_{};
    size_t differenceGeneration_{};
    std::shared_ptr<const std::vector<float>> diffValues_;
    std::shared_ptr<Raster> diffRaster_;
    std::wstring status_;
    Modal modal_{};
    HWND modalWindow_{};
    bool tooltipReady_{};
    std::optional<Hit> context_;
    Point contextPoint_;
    size_t focusIndex_{};
    bool keyboardFocus_{};
    std::atomic_size_t activeDecode_{};
    HWND completionWindow_{};
    std::mutex completionLock_;
    std::deque<std::function<void()>> completed_;
    Worker measurementWorker_;
    Worker differenceWorker_;
    Worker decodeWorker_;
    Worker thumbWorker_;

    void invalidate();
    void post(std::function<void()>);
    void drain();
    void status(std::wstring message);
    void save();
    void restore();
    void addFolder(fs::path);
    void scan(Column&);
    void sort(Column&);
    void refresh();
    void pickFolder();
    TreeNode* findTree(const fs::path&, std::vector<TreeNode>&);
    void expand(TreeNode&);
    void showInTree(const fs::path&);
    void requestThumb(Entry&);
    void trimThumbs(size_t first, size_t last, size_t left, size_t right);
    void select(const std::vector<fs::path>&, bool ctrl);
    void beginCompare();
    void openCompare(const std::vector<std::pair<size_t, size_t>>&);
    void load();
    void step(int delta);
    void back();
    void reset();
    void split();
    void closeSecond();
    void recompute();
    void updateDifference(bool recalculate);
    void chooseDelete(std::vector<fs::path>);
    void removeFiles(bool permanent);
    void setModal(Modal);
    void command(const Hit&, bool ctrl = false);
    void key(Surface&, WPARAM, LPARAM, bool down);
    void beginPointer(Surface&, Point, UINT, WPARAM keys);
    void movePointer(Surface&, Point);
    void endPointer(Surface&, Point);
    void wheel(Surface&, Point, double, bool horizontal);
    void clearDrag();
    std::optional<ReorderDrop> reorderDrop(const Surface&, Point) const;
    std::optional<Pane> paneAt(Surface&, Point) const;
    View view(const Surface&, const Pane&, size_t imageSlot) const;
    size_t visibleSlot(size_t paneSlot) const;
    Point normal(const Surface&, const Pane&, Point) const;
    bool roiAt(const Surface&, const Pane&, Point, unsigned& edges) const;

    bool target(Surface&);
    void discardTarget(Surface&);
    void paint(Surface&);
    void fill(Surface&, Box, D2D1_COLOR_F, float radius = 0);
    void outline(Surface&, Box, D2D1_COLOR_F, float radius = 0, float width = 1);
    void line(Surface&, Point, Point, D2D1_COLOR_F, float width = 1);
    void label(Surface&, const std::wstring&, Box, int size = 14, D2D1_COLOR_F = color.fg,
        DWRITE_TEXT_ALIGNMENT = DWRITE_TEXT_ALIGNMENT_LEADING, bool bold = false, bool wrap = false,
        bool shadow = false);
    void button(Surface&, Box, const std::wstring&, Command, int style = 0, size_t a = 0, size_t b = 0,
        std::wstring tip = {});
    void bitmap(Surface&, const std::shared_ptr<Raster>&, Point top, double scale, bool nearest, Orientation = {});
    void fitBitmap(Surface&, const std::shared_ptr<Raster>&, Box);
    void drawBrowser(Surface&);
    void drawTree(Surface&, const std::vector<TreeNode>&, size_t level, size_t& row, Box clip);
    void drawCompare(Surface&);
    void drawPane(Surface&, const Pane&);
    void drawHistogram(Surface&, const Image&, Box);
    void drawModal(Surface&);
    void drawContext(Surface&);
    void tooltip(Surface&);
};

void App::invalidate() {
    if (main_.hwnd) InvalidateRect(main_.hwnd, nullptr, FALSE);
    if (second_.hwnd) InvalidateRect(second_.hwnd, nullptr, FALSE);
}

void App::post(std::function<void()> task) {
    {
        std::lock_guard guard(completionLock_);
        completed_.emplace_back(std::move(task));
    }
    PostMessageW(completionWindow_, workReady, 0, 0);
}

void App::drain() {
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard guard(completionLock_);
        tasks.swap(completed_);
    }
    for (auto& task : tasks) task();
    invalidate();
}

void App::status(std::wstring message) {
    status_ = std::move(message);
    KillTimer(main_.hwnd, 1);
    SetTimer(main_.hwnd, 1, 3600, nullptr);
    invalidate();
}

bool App::create(const std::vector<fs::path>& folders) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_.GetAddressOf())))) return false;
    for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
        if (!(GetLogicalDrives() & (1u << (drive - L'A')))) continue;
        std::wstring path{drive, L':', L'\\'};
        tree_.push_back({path, path, {}, false, false});
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = App::proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(instance_, MAKEINTRESOURCE(101));
    wc.lpszClassName = windowClass;
    if (!RegisterClassExW(&wc)) return false;

    main_.hwnd = CreateWindowExW(0, windowClass, L"PicCompare · 图像比对", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr, instance_, this);
    if (!main_.hwnd) return false;
    main_.dpi = (std::max)(96u, GetDpiForWindow(main_.hwnd)) / 96.;
    completionWindow_ = main_.hwnd;
    DragAcceptFiles(main_.hwnd, TRUE);
    if (folders.empty()) restore();
    else for (const auto& folder : folders) addFolder(folder);
    if (!currentFolder_.empty()) showInTree(currentFolder_);
    ShowWindow(main_.hwnd, SW_SHOW);
    UpdateWindow(main_.hwnd);
    return true;
}

void App::save() {
    if (!persist_) return;
    std::wstring directories;
    for (const auto& col : columns_) {
        directories += col.path.wstring();
        directories += L'\0';
    }
    if (directories.empty()) directories += L'\0';
    directories += L'\0';
    RegSetKeyValueW(HKEY_CURRENT_USER, preferences, L"Folders", REG_MULTI_SZ,
        directories.data(), DWORD(directories.size() * sizeof(wchar_t)));
    const auto current = currentFolder_.wstring();
    RegSetKeyValueW(HKEY_CURRENT_USER, preferences, L"LastFolder", REG_SZ,
        current.c_str(), DWORD((current.size() + 1) * sizeof(wchar_t)));
}

void App::restore() {
    if (!persist_) return;
    DWORD count{};
    if (RegGetValueW(HKEY_CURRENT_USER, preferences, L"LastFolder", RRF_RT_REG_SZ,
        nullptr, nullptr, &count) == ERROR_SUCCESS) {
        std::wstring value(count / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, preferences, L"LastFolder", RRF_RT_REG_SZ,
            nullptr, value.data(), &count) == ERROR_SUCCESS) currentFolder_ = fs::path(value.c_str());
    }
    const fs::path last = currentFolder_;
    if (RegGetValueW(HKEY_CURRENT_USER, preferences, L"Folders", RRF_RT_REG_MULTI_SZ,
        nullptr, nullptr, &count) == ERROR_SUCCESS) {
        std::wstring value(count / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, preferences, L"Folders", RRF_RT_REG_MULTI_SZ,
            nullptr, value.data(), &count) == ERROR_SUCCESS) {
            for (const wchar_t* item = value.c_str(); *item; item += wcslen(item) + 1) {
                std::error_code ec;
                if (fs::is_directory(item, ec)) addFolder(item);
            }
        }
    }
    std::error_code pathError;
    if (!last.empty() && fs::is_directory(last, pathError)) currentFolder_ = last;
}

void App::sort(Column& col) {
    std::sort(col.files.begin(), col.files.end(), [this](const Entry& a, const Entry& b) {
        if (byTime_ && a.time != b.time) return descending_ ? a.time > b.time : a.time < b.time;
        int cmp = StrCmpLogicalW(a.path.filename().c_str(), b.path.filename().c_str());
        if (!cmp) cmp = a.path.compare(b.path);
        return descending_ ? cmp > 0 : cmp < 0;
    });
}

void App::scan(Column& col) {
    col.files.clear();
    col.error.clear();
    std::error_code ec;
    fs::directory_iterator it(col.path, ec), end;
    while (!ec && it != end) {
        const auto path = it->path();
        std::error_code entryError;
        if (it->is_regular_file(entryError) && supported(path) && !col.hidden.contains(path)) {
            Entry entry;
            entry.path = path;
            entry.size = it->file_size(entryError);
            if (entryError) entry.size = 0;
            entry.time = it->last_write_time(entryError);
            col.files.push_back(std::move(entry));
        }
        it.increment(ec);
    }
    if (ec) col.error = L"无法完整读取此目录。";
    sort(col);
}

void App::addFolder(fs::path path) {
    std::error_code ec;
    path = fs::weakly_canonical(path, ec);
    if (ec || !fs::is_directory(path, ec)) {
        status(L"无法打开该文件夹。");
        return;
    }
    currentFolder_ = path;
    const auto found = std::find_if(columns_.begin(), columns_.end(), [&](const Column& col) {
        return CompareStringOrdinal(col.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (found != columns_.end()) {
        status(L"这个文件夹已在预览中。");
        return;
    }
    Column col;
    col.path = path;
    scan(col);
    if (!col.files.empty()) selection_.insert(col.files.front().path);
    columns_.push_back(std::move(col));
    invalidate();
}

void App::refresh() {
    const auto previous = selection_;
    selection_.clear();
    for (auto& col : columns_) {
        std::unordered_map<fs::path, size_t> previousOrder;
        if (col.manualOrder)
            for (size_t row = 0; row < col.files.size(); ++row) previousOrder.emplace(col.files[row].path, row);
        col.hidden.clear();
        scan(col);
        if (!previousOrder.empty()) {
            const auto rank = [&](const Entry& entry) {
                const auto found = previousOrder.find(entry.path);
                return found == previousOrder.end() ? previousOrder.size() : found->second;
            };
            std::stable_sort(col.files.begin(), col.files.end(), [&](const Entry& a, const Entry& b) {
                return rank(a) < rank(b);
            });
        }
        for (const auto& entry : col.files)
            if (previous.contains(entry.path)) selection_.insert(entry.path);
    }
    status(L"已重新载入文件夹。");
}

void App::pickFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)))) {
        status(L"无法打开文件夹选择窗口。");
        return;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
    dialog->SetTitle(L"添加比较文件夹");
    ComPtr<IShellItem> initial;
    if (!currentFolder_.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(currentFolder_.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        dialog->SetFolder(initial.Get());
    if (FAILED(dialog->Show(main_.hwnd))) return;
    ComPtr<IShellItemArray> selected;
    if (FAILED(dialog->GetResults(&selected))) return;
    DWORD count{};
    selected->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path{};
        if (SUCCEEDED(selected->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            addFolder(path);
            CoTaskMemFree(path);
        }
    }
    showInTree(currentFolder_);
}

TreeNode* App::findTree(const fs::path& path, std::vector<TreeNode>& nodes) {
    for (auto& node : nodes) {
        if (node.path == path) return &node;
        if (auto* found = findTree(path, node.children)) return found;
    }
    return nullptr;
}

void App::expand(TreeNode& node) {
    node.expanded = true;
    if (node.scanned) return;
    node.scanned = true;
    std::error_code ec;
    fs::directory_iterator it(node.path, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code info;
        if (it->is_directory(info)) {
            const auto path = it->path();
            node.children.push_back({path, path.filename().wstring(), {}, false, false});
        }
        it.increment(ec);
    }
    std::sort(node.children.begin(), node.children.end(), [](const TreeNode& a, const TreeNode& b) {
        return StrCmpLogicalW(a.label.c_str(), b.label.c_str()) < 0;
    });
}

void App::showInTree(const fs::path& path) {
    std::vector<fs::path> parents;
    for (auto p = path.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        parents.push_back(p);
    if (!path.root_path().empty()) parents.push_back(path.root_path());
    for (auto it = parents.rbegin(); it != parents.rend(); ++it)
        if (auto* node = findTree(*it, tree_)) expand(*node);
}

void App::requestThumb(Entry& entry) {
    if (entry.requested) return;
    entry.requested = true;
    const fs::path path = entry.path;
    const auto time = entry.time;
    thumbWorker_.push([this, path, time](IWICImagingFactory* wic) {
        auto thumb = thumbnail(wic, path);
        post([this, path, time, thumb = std::move(thumb)] {
            for (auto& col : columns_) {
                if (col.path != path.parent_path()) continue;
                for (auto& candidate : col.files) {
                    if (candidate.path != path || candidate.time != time) continue;
                    candidate.thumb = thumb;
                    candidate.broken = !thumb;
                    candidate.requested = true;
                    break;
                }
            }
        });
    });
}

void App::trimThumbs(size_t first, size_t last, size_t left, size_t right) {
    constexpr size_t margin = 20;
    const size_t lower = first > margin ? first - margin : 0;
    for (size_t index = 0; index < columns_.size(); ++index) {
        auto& col = columns_[index];
        for (size_t row = 0; row < col.files.size(); ++row) {
            if (index >= left && index <= right && row >= lower && row <= last + margin) continue;
            auto& entry = col.files[row];
            if (entry.thumb) {
                entry.thumb.reset();
                entry.requested = false;
            }
        }
    }
}

void App::select(const std::vector<fs::path>& paths, bool ctrl) {
    if (paths.empty()) return;
    const bool all = std::all_of(paths.begin(), paths.end(), [this](const fs::path& p) {
        return selection_.contains(p);
    });
    if (!ctrl) selection_.clear();
    if (ctrl && all) for (const auto& p : paths) selection_.erase(p);
    else selection_.insert(paths.begin(), paths.end());
    invalidate();
}

void App::beginCompare() {
    pendingSelection_.clear();
    for (size_t column = 0; column < columns_.size(); ++column)
        for (size_t row = 0; row < columns_[column].files.size(); ++row)
            if (selection_.contains(columns_[column].files[row].path)) pendingSelection_.emplace_back(column, row);
    if (pendingSelection_.empty()) {
        status(L"请先选中要对比的图片。");
    } else if (pendingSelection_.size() > maxCompare) {
        rangeStart_ = 0;
        setModal(Modal::TooMany);
    } else {
        openCompare(pendingSelection_);
    }
}

void App::openCompare(const std::vector<std::pair<size_t, size_t>>& chosen) {
    comparing_ = true;
    modal_ = Modal::None;
    status_.clear();
    slots_.clear();
    for (const auto& [column, row] : chosen) {
        Slot slot;
        slot.column = column;
        slot.row = row;
        slots_.push_back(std::move(slot));
    }
    roi_.reset();
    hotSlot_ = 0;
    reset();
    load();
}

void App::load() {
    const size_t request = ++generation_;
    activeDecode_ = request;
    ++measurementGeneration_;
    std::map<fs::path, std::shared_ptr<Image>> reuse;
    for (const auto& slot : slots_)
        if (slot.image && slot.image->error.empty()) reuse.emplace(slot.path, slot.image);
    diffValues_.reset();
    diffRaster_.reset();
    for (size_t i = 0; i < slots_.size(); ++i) {
        auto& slot = slots_[i];
        slot.path = columns_[slot.column].files[slot.row].path;
        slot.measurement.reset();
        const auto found = reuse.find(slot.path);
        slot.image = found == reuse.end() ? nullptr : found->second;
    }
    std::vector<std::pair<size_t, fs::path>> paths;
    for (size_t i = 0; i < slots_.size(); ++i)
        if (!slots_[i].image) paths.emplace_back(i, slots_[i].path);
    decodeWorker_.push([this, request, paths = std::move(paths)](IWICImagingFactory* wic) {
        for (const auto& [i, path] : paths) {
            if (request != activeDecode_.load()) break;
            auto image = decode(wic, path);
            post([this, request, i, image = std::move(image)] {
                if (request != generation_) return;
                slots_[i].image = image;
                recompute();
                updateDifference(true);
            });
        }
    }, true, true);
    recompute();
    updateDifference(true);
    invalidate();
}

void App::step(int delta) {
    for (const auto& slot : slots_) {
        const auto size = columns_[slot.column].files.size();
        if ((delta < 0 && slot.row == 0) || (delta > 0 && slot.row + 1 >= size)) {
            status(delta < 0 ? L"已到第一组。" : L"已到最后一组。");
            return;
        }
    }
    for (auto& slot : slots_) slot.row = size_t(ptrdiff_t(slot.row) + delta);
    status_.clear();
    load();
}

void App::back() {
    ++generation_;
    activeDecode_ = generation_;
    ++measurementGeneration_;
    ++differenceGeneration_;
    comparing_ = false;
    modal_ = Modal::None;
    modalWindow_ = nullptr;
    keyboardFocus_ = cHeld_ = sHeld_ = false;
    clearDrag();
    closeSecond();
    slots_.clear();
    roi_.reset();
    diffValues_.reset();
    diffRaster_.reset();
    probe_.reset();
    status_.clear();
    invalidate();
}

void App::reset() {
    ++differenceGeneration_;
    zoom_ = gain_ = 1;
    actual_ = diffOn_ = cHeld_ = false;
    groupPan_ = {};
    zoomShift_ = {};
    for (auto& slot : slots_) slot.offset = {};
    diffValues_.reset();
    diffRaster_.reset();
    probe_.reset();
    invalidate();
}

void App::closeSecond() {
    split_ = false;
    if (second_.hwnd) DestroyWindow(second_.hwnd);
    second_ = {};
    if (mainBeforeSplit_) {
        SetWindowPlacement(main_.hwnd, &*mainBeforeSplit_);
        mainBeforeSplit_.reset();
    }
    invalidate();
}

void App::split() {
    if (split_) {
        closeSecond();
        return;
    }
    if (slots_.size() < 2) {
        status(L"请先选择至少两张图片。");
        return;
    }
    struct Monitors { HMONITOR current, other{}; } monitors{MonitorFromWindow(main_.hwnd, MONITOR_DEFAULTTONEAREST)};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& m = *reinterpret_cast<Monitors*>(data);
        if (monitor == m.current) return TRUE;
        m.other = monitor;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&monitors));
    if (!monitors.other) {
        status(L"未检测到第二个扩展显示器。");
        return;
    }
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitors.other, &info)) return;
    const auto r = info.rcWork;
    second_.secondary = true;
    second_.hwnd = CreateWindowExW(0, windowClass, L"PicCompare · 第二屏", WS_OVERLAPPEDWINDOW,
        r.left + 16, r.top + 16, r.right - r.left - 32, r.bottom - r.top - 32,
        nullptr, nullptr, instance_, this);
    if (!second_.hwnd) {
        status(L"无法打开第二屏窗口。");
        return;
    }
    second_.dpi = (std::max)(96u, GetDpiForWindow(second_.hwnd)) / 96.;
    split_ = true;
    splitBoundary_ = (slots_.size() + 1) / 2;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (GetWindowPlacement(main_.hwnd, &placement)) mainBeforeSplit_ = placement;
    ShowWindow(main_.hwnd, SW_MAXIMIZE);
    ShowWindow(second_.hwnd, SW_MAXIMIZE);
    invalidate();
}

void App::recompute() {
    const auto request = ++measurementGeneration_;
    if (!roi_) {
        for (auto& slot : slots_) slot.measurement.reset();
        return;
    }
    struct Input { std::shared_ptr<Image> image; Orientation orientation; };
    std::vector<Input> inputs;
    for (const auto& slot : slots_) inputs.push_back({slot.image, slot.orientation});
    measurementWorker_.push([this, request, inputs = std::move(inputs), roi = *roi_](IWICImagingFactory*) {
        std::vector<std::optional<Stats>> results(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (!inputs[i].image || !inputs[i].image->display) continue;
            results[i] = measure(*inputs[i].image, inputs[i].orientation, roi.boxes[i]);
        }
        post([this, request, results = std::move(results)]() mutable {
            if (request != measurementGeneration_) return;
            for (size_t i = 0; i < slots_.size(); ++i) slots_[i].measurement = results[i];
        });
    }, true, true);
}

void App::updateDifference(bool recalculate) {
    const size_t request = ++differenceGeneration_;
    if (recalculate) {
        diffValues_.reset();
        diffRaster_.reset();
    }
    if (!diffOn_ || slots_.size() != 2 || !slots_[0].image || !slots_[0].image->display ||
        !slots_[1].image || !slots_[1].image->display) {
        diffRaster_.reset();
        return;
    }
    const auto left = slots_[0].image, right = slots_[1].image;
    const auto a = slots_[0].orientation, b = slots_[1].orientation;
    const double gain = gain_;
    differenceWorker_.push([this, request, left, right, a, b, gain, values = diffValues_](IWICImagingFactory*) mutable {
        try {
            if (!values) values = std::make_shared<const std::vector<float>>(difference(*left, a, *right, b));
            auto raster = differenceRaster(*right, *values, gain);
            post([this, request, values = std::move(values), raster = std::move(raster)] {
                if (request != differenceGeneration_) return;
                diffValues_ = values;
                diffRaster_ = raster;
            });
        } catch (const std::bad_alloc&) {
            post([this, request] {
                if (request != differenceGeneration_) return;
                diffOn_ = false;
                diffValues_.reset();
                diffRaster_.reset();
                status(L"内存不足，无法显示差值。");
            });
        }
    }, true, true);
}
bool App::target(Surface& s) {
    RECT client{};
    if (!GetClientRect(s.hwnd, &client) || client.right <= 0 || client.bottom <= 0) return false;
    const auto pixels = D2D1::SizeU(client.right, client.bottom);
    if (const UINT dpi = GetDpiForWindow(s.hwnd)) s.dpi = dpi / 96.;
    const float dpi = float(s.dpi * 96);
    if (s.target) {
        // Direct2D stretches the back buffer when its pixel size differs from the client area.
        const auto current = s.target->GetPixelSize();
        if (current.width == pixels.width && current.height == pixels.height) {
            s.target->SetDpi(dpi, dpi);
            return true;
        }
        if (SUCCEEDED(s.target->Resize(pixels))) {
            s.target->SetDpi(dpi, dpi);
            return true;
        }
        discardTarget(s);
    }
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(), dpi, dpi);
    if (FAILED(d2d_->CreateHwndRenderTarget(properties, D2D1::HwndRenderTargetProperties(s.hwnd,
        pixels), &s.target))) return false;
    if (FAILED(s.target->CreateSolidColorBrush(color.fg, &s.brush))) {
        discardTarget(s);
        return false;
    }
    return true;
}

void App::discardTarget(Surface& s) {
    s.bitmaps.clear();
    s.brush.Reset();
    s.target.Reset();
}

void App::fill(Surface& s, Box b, D2D1_COLOR_F c, float radius) {
    if (b.width() <= 0 || b.height() <= 0) return;
    s.brush->SetColor(c);
    if (radius > 0) s.target->FillRoundedRectangle(D2D1::RoundedRect(rect(b), radius, radius), s.brush.Get());
    else s.target->FillRectangle(rect(b), s.brush.Get());
}

void App::outline(Surface& s, Box b, D2D1_COLOR_F c, float radius, float width) {
    s.brush->SetColor(c);
    s.target->DrawRoundedRectangle(D2D1::RoundedRect(rect(b), radius, radius), s.brush.Get(), width);
}

void App::line(Surface& s, Point a, Point b, D2D1_COLOR_F c, float width) {
    s.brush->SetColor(c);
    s.target->DrawLine(point(a), point(b), s.brush.Get(), width);
}

void App::label(Surface& s, const std::wstring& text, Box box, int size, D2D1_COLOR_F ink,
    DWRITE_TEXT_ALIGNMENT alignment, bool bold, bool wrap, bool shadow) {
    if (box.width() <= 0 || box.height() <= 0 || text.empty()) return;
    const int id = size * 16 + int(alignment) * 4 + (bold ? 2 : 0) + (wrap ? 1 : 0);
    auto& format = fonts_[id];
    if (!format) {
        if (FAILED(write_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, float(size), L"zh-CN", &format))) return;
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(write_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
                format->SetTrimming(&trim, ellipsis.Get());
        }
    }
    if (shadow) {
        s.brush->SetColor(D2D1::ColorF(0x080a0e, .95f));
        const double offset = 1.25 / s.dpi;
        for (const auto& shift : {Point{-offset, 0}, Point{offset, 0}, Point{0, -offset}, Point{0, offset}}) {
            const Box edge{box.l + shift.x, box.t + shift.y, box.r + shift.x, box.b + shift.y};
            s.target->DrawText(text.data(), UINT32(text.size()), format.Get(), rect(edge), s.brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    s.brush->SetColor(ink);
    s.target->DrawText(text.data(), UINT32(text.size()), format.Get(), rect(box), s.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::button(Surface& s, Box b, const std::wstring& text, Command action, int style, size_t a,
    size_t index, std::wstring tip) {
    const bool hover = b.contains(s.mouse);
    const auto background = style == 2 ? color.danger : style == 1 ? color.primary :
        hover ? color.selected : color.surface;
    fill(s, b, background, 7);
    if (hover) outline(s, inset(b, .5), style == 2 ? color.pink : color.blue, 7);
    if (keyboardFocus_ && focusIndex_ == s.hits.size()) outline(s, inset(b, 2), color.blue, 5);
    label(s, text, inset(b, 3), 13, style ? color.fg : color.muted, DWRITE_TEXT_ALIGNMENT_CENTER, style != 0);
    s.hits.push_back({b, action, a, index, std::move(tip)});
}

void App::bitmap(Surface& s, const std::shared_ptr<Raster>& raster, Point top, double scale,
    bool nearest, Orientation o) {
    if (!raster || scale <= 0) return;
    const auto origin = o.offset(raster->w, raster->h);
    const auto zoom = float(scale);
    const auto transform = D2D1::Matrix3x2F(o.xx * zoom, o.yx * zoom, o.xy * zoom, o.yy * zoom,
        float(top.x + origin.x * scale), float(top.y + origin.y * scale));
    s.target->SetTransform(transform);
    const int edge = int((std::min)(4096u, s.target->GetMaximumBitmapSize()));
    const int columns = (raster->w + edge - 1) / edge;
    const int rows = (raster->h + edge - 1) / edge;
    auto& tiles = s.bitmaps[raster.get()];
    if (tiles.empty()) tiles.resize(size_t(columns) * rows);
    for (int ty = 0; ty < rows; ++ty) for (int tx = 0; tx < columns; ++tx) {
        const int x = tx * edge, y = ty * edge;
        const int w = (std::min)(edge, raster->w - x), h = (std::min)(edge, raster->h - y);
        auto& tile = tiles[size_t(ty) * columns + tx];
        if (tile.source.lock() != raster) {
            tile.bitmap.Reset();
            tile.source = raster;
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            s.target->CreateBitmap(D2D1::SizeU(w, h), &raster->bgra[size_t(y) * raster->w + x],
                UINT32(raster->w * 4), properties, &tile.bitmap);
        }
        if (tile.bitmap) s.target->DrawBitmap(tile.bitmap.Get(), D2D1::RectF(float(x), float(y), float(x + w), float(y + h)),
            1, nearest ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    s.target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void App::fitBitmap(Surface& s, const std::shared_ptr<Raster>& raster, Box area) {
    const double scale = fit(area, raster->w, raster->h);
    const auto middle = area.center();
    const double w = raster->w * scale / 2, h = raster->h * scale / 2;
    bitmap(s, raster, {middle.x - w, middle.y - h}, scale, false);
}

void App::drawTree(Surface& s, const std::vector<TreeNode>& nodes, size_t level, size_t& row, Box clip) {
    for (const auto& node : nodes) {
        const double y = clip.t + row * 30 - treeY_;
        ++row;
        if (y + 30 > clip.t && y < clip.b) {
            const Box b{clip.l + 8, y + 1, clip.r - 8, y + 29};
            const auto index = treePaths_.size();
            treePaths_.push_back(node.path);
            const double indent = (std::min)(b.width() - 36, 8. + level * 14);
            if (node.path == currentFolder_ || b.contains(s.mouse)) fill(s, b, color.selected, 5);
            label(s, node.expanded ? L"⌄" : L"›", {b.l + indent, b.t, b.l + indent + 18, b.b}, 15, color.blue);
            label(s, node.label, {b.l + indent + 19, b.t, b.r - 4, b.b}, 13,
                node.path == currentFolder_ ? color.fg : color.muted);
            Box toggle{b.l, (std::max)(b.t, clip.t), b.l + indent + 18, (std::min)(b.b, clip.b)};
            Box open{toggle.r, toggle.t, b.r, toggle.b};
            s.hits.push_back({toggle, Command::TreeToggle, index, 0, {}});
            s.hits.push_back({open, Command::TreeOpen, index, 0, node.path.wstring()});
        }
        if (node.expanded) drawTree(s, node.children, level + 1, row, clip);
    }
}

void App::drawBrowser(Surface& s) {
    const double side = treeVisible_ ? 216 : 0;
    const double lower = s.height - footer;
    fill(s, {0, 0, s.width, toolbar}, color.panel);
    line(s, {0, toolbar - .5}, {s.width, toolbar - .5}, color.line);
    fill(s, {16, 16, 47, 47}, color.primary, 9);
    outline(s, {23, 24, 40, 39}, color.fg, 3, 1.4f);
    line(s, {31.5, 25}, {31.5, 38}, color.fg, 1.4f);
    label(s, L"PicCompare", {57, 10, 180, 53}, 20, color.fg, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    button(s, {185, 14, 320, 50}, L"＋ 添加文件夹", Command::Add, 1, 0, 0, L"点击选择文件夹，也可把文件夹拖到这里");
    const bool hasManualOrder = std::any_of(columns_.begin(), columns_.end(), [](const Column& col) {
        return col.manualOrder;
    });
    button(s, {332, 14, 408, 50}, L"文件名", Command::SortName, !byTime_ && !hasManualOrder ? 1 : 0);
    button(s, {413, 14, 487, 50}, L"修改时间", Command::SortTime, byTime_ && !hasManualOrder ? 1 : 0);
    button(s, {492, 14, 552, 50}, descending_ ? L"降序 ↓" : L"升序 ↑", Command::Order);
    button(s, {565, 14, 637, 50}, L"重新载入", Command::Refresh);
    button(s, {s.width - 91, 14, s.width - 16, 50}, L"快捷键 K", Command::Help);

    if (treeVisible_) {
        fill(s, {0, toolbar, side, lower}, color.panel);
        label(s, L"文件夹", {17, toolbar + 4, side - 58, toolbar + 43}, 14, color.fg,
            DWRITE_TEXT_ALIGNMENT_LEADING, true);
        button(s, {side - 54, toolbar + 11, side - 13, toolbar + 39}, L"收起", Command::ToggleTree);
        treePaths_.clear();
        size_t treeRow{};
        const Box treeClip{0, toolbar + 51, side, lower};
        s.target->PushAxisAlignedClip(rect(treeClip), D2D1_ANTIALIAS_MODE_ALIASED);
        drawTree(s, tree_, 0, treeRow, treeClip);
        s.target->PopAxisAlignedClip();
        treeRows_ = treeRow;
        line(s, {side - .5, toolbar}, {side - .5, lower}, color.line);
    }

    if (columns_.empty()) {
        const auto c = Box{side, toolbar, s.width, lower}.center();
        const Box card{c.x - 228, c.y - 105, c.x + 228, c.y + 105};
        fill(s, card, color.panel, 18);
        outline(s, card, color.line, 18);
        label(s, L"添加文件夹开始比对", {card.l + 25, c.y - 68, card.r - 25, c.y - 22}, 23, color.fg,
            DWRITE_TEXT_ALIGNMENT_CENTER, true);
        label(s, L"可从资源管理器或左侧文件夹树拖入目录", {card.l + 14, c.y - 19, card.r - 14, c.y + 19},
            14, color.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        button(s, {c.x - 75, c.y + 37, c.x + 75, c.y + 75}, L"＋ 添加文件夹", Command::Add, 1);
    } else {
        const double beginX = side + gutter;
        const double beginY = toolbar + header;
        const double rowBottom = lower - 8;
        const double columnWidth = std::clamp((s.width - beginX - 10) / columns_.size(), 226., 520.);
        size_t totalRows{};
        for (const auto& column : columns_) totalRows = (std::max)(totalRows, column.files.size());
        scrollY_ = std::clamp(scrollY_, 0., (std::max)(0., totalRows * rowHeight - (rowBottom - beginY)));
        scrollX_ = std::clamp(scrollX_, 0., (std::max)(0., columns_.size() * columnWidth - (s.width - beginX - 8)));
        const size_t first = size_t(scrollY_ / rowHeight);
        const size_t last = (std::min)(totalRows, size_t((scrollY_ + (std::max)(0., rowBottom - beginY)) / rowHeight) + 1);
        trimThumbs(first, last, size_t(scrollX_ / columnWidth),
            size_t((scrollX_ + s.width - beginX) / columnWidth));
        fill(s, {side, toolbar, s.width, beginY}, color.panel);
        label(s, L"行", {side, toolbar, beginX, beginY}, 13, color.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        line(s, {side, beginY - .5}, {s.width, beginY - .5}, color.line);
        const Box grid{beginX, toolbar, s.width - 7, rowBottom};
        s.grid = {grid, beginY, columnWidth};
        s.target->PushAxisAlignedClip(rect(grid), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t col = 0; col < columns_.size(); ++col) {
            auto& column = columns_[col];
            const double x = beginX + col * columnWidth - scrollX_;
            if (x + columnWidth <= grid.l || x >= grid.r) continue;
            const Box heading{x, toolbar, x + columnWidth, beginY};
            if (drag_.mode == Gesture::ColumnDrag && drag_.moved && drag_.reorder->a == col)
                fill(s, heading, color.selected);
            const Box title{x + 25, toolbar + 3, x + columnWidth - 75, toolbar + 34};
            const auto name = column.path.filename().empty() ? column.path.wstring() : column.path.filename().wstring();
            for (int i = 0; i < 3; ++i) {
                const double y = toolbar + 20 + i * 4;
                fill(s, {x + 10, y, x + 12, y + 2}, color.muted, 1);
                fill(s, {x + 15, y, x + 17, y + 2}, color.muted, 1);
            }
            label(s, name, title, 15, color.fg, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            label(s, std::to_wstring(column.files.size()) + L" 张图片" + (column.manualOrder ? L" · 手动排序" : L""),
                {title.l, toolbar + 30, title.r, beginY - 3},
                12, color.muted);
            s.hits.push_back({{(std::max)(heading.l, grid.l), heading.t, (std::min)(heading.r, grid.r), heading.b},
                Command::ColumnPath, col, 0, column.path.wstring()});
            button(s, {x + columnWidth - 72, toolbar + 17, x + columnWidth - 35, toolbar + 43}, L"全选", Command::ColumnSelect,
                0, col, 0, L"选中这一列的全部图片");
            button(s, {x + columnWidth - 31, toolbar + 17, x + columnWidth - 7, toolbar + 43}, L"×", Command::ColumnClose,
                0, col, 0, L"移除此文件夹列");
            line(s, {x + columnWidth - .5, toolbar}, {x + columnWidth - .5, rowBottom}, color.line);
            s.target->PushAxisAlignedClip(rect({grid.l, beginY, grid.r, rowBottom}), D2D1_ANTIALIAS_MODE_ALIASED);
            if (column.files.empty())
                label(s, column.error.empty() ? L"没有可预览的图片" : column.error,
                    {x + 12, beginY + 25, x + columnWidth - 10, beginY + 65}, 13, color.muted);
            for (size_t row = first; row < last && row < column.files.size(); ++row) {
                auto& entry = column.files[row];
                requestThumb(entry);
                const double y = beginY + row * rowHeight - scrollY_;
                const Box card{x + 6, y + 3, x + columnWidth - 6, y + rowHeight - 3};
                const Box art{card.l + 4, card.t + 4, card.l + 64, card.b - 4};
                const bool selected = selection_.contains(entry.path);
                const bool hovered = card.contains(s.mouse);
                const bool dragging = drag_.mode == Gesture::FileDrag && drag_.moved &&
                    drag_.reorder->a == col && drag_.reorder->b == row;
                fill(s, card, selected || dragging ? color.selected : hovered ? color.surface : color.panel, 6);
                fill(s, art, color.canvas, 4);
                if (entry.thumb) fitBitmap(s, entry.thumb, inset(art, 2));
                else label(s, entry.broken ? L"无法解码" : L"载入中…", art, 10,
                    entry.broken ? color.pink : color.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
                const Box mark{card.r - 22, card.center().y - 7, card.r - 8, card.center().y + 7};
                if (selected) {
                    outline(s, inset(card, .5), color.blue, 6);
                    fill(s, mark, color.primary, 3);
                    label(s, L"✓", mark, 10, color.fg, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                const auto size = bytes(entry.size);
                const double sizeWidth = (std::max)(41., size.size() * 6.5);
                const Box sizeBox{mark.l - sizeWidth - 4, card.t, mark.l - 4, card.b};
                label(s, entry.path.filename().wstring(), {art.r + 8, card.t, sizeBox.l - 5, card.b}, 13);
                label(s, size, sizeBox, 11, color.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
                s.hits.push_back({{(std::max)(card.l, grid.l), (std::max)(card.t, beginY),
                    (std::min)(card.r, grid.r), (std::min)(card.b, rowBottom)}, Command::FileSelect,
                    col, row, entry.path.filename().wstring()});
            }
            s.target->PopAxisAlignedClip();
        }
        if (const auto drop = reorderDrop(s, s.mouse)) fill(s, drop->indicator, color.blue, 1);
        s.target->PopAxisAlignedClip();
        for (auto& hit : s.hits) {
            if (hit.action != Command::ColumnSelect && hit.action != Command::ColumnClose) continue;
            hit.box.l = (std::max)(hit.box.l, grid.l);
            hit.box.r = (std::min)(hit.box.r, grid.r);
        }
        s.target->PushAxisAlignedClip(rect({side, beginY, beginX, rowBottom}), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t row = first; row < last; ++row) {
            const double y = beginY + row * rowHeight - scrollY_;
            const Box b{side + 5, y + 3, beginX - 5, y + rowHeight - 3};
            const bool selected = std::all_of(columns_.begin(), columns_.end(), [&](const Column& col) {
                return row >= col.files.size() || selection_.contains(col.files[row].path);
            });
            if (selected || b.contains(s.mouse)) fill(s, b, color.selected, 6);
            label(s, std::to_wstring(row + 1), b, 12, selected ? color.blue : color.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
            s.hits.push_back({{b.l, (std::max)(b.t, beginY), b.r, (std::min)(b.b, rowBottom)},
                Command::RowSelect, row, 0, L"选中这一行"});
        }
        s.target->PopAxisAlignedClip();
        if (totalRows * rowHeight > rowBottom - beginY) {
            const double extent = rowBottom - beginY, track = extent / (totalRows * rowHeight);
            fill(s, {s.width - 5, beginY + scrollY_ * track, s.width - 2, beginY + (scrollY_ + extent) * track}, color.line, 1);
        }
        if (columns_.size() * columnWidth > grid.width()) {
            const double factor = grid.width() / (columns_.size() * columnWidth);
            fill(s, {grid.l + scrollX_ * factor, rowBottom + 3,
                grid.l + (scrollX_ + grid.width()) * factor, rowBottom + 6}, color.line, 1);
        }
    }

    fill(s, {0, lower, s.width, s.height}, color.panel);
    line(s, {0, lower + .5}, {s.width, lower + .5}, color.line);
    if (!treeVisible_) button(s, {12, lower + 10, 90, lower + 43}, L"文件夹树", Command::ToggleTree);
    const double start = treeVisible_ ? 18 : 103;
    label(s, L"已选 " + std::to_wstring(selection_.size()) + L" 张", {start, lower + 1, start + 117, s.height},
        14, color.fg, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    label(s, status_.empty() ? L"Ctrl 多选   ·   拖列标题左右换位   ·   拖图片行上下排序" : status_,
        {start + 118, lower + 1, s.width - 166, s.height}, 12, status_.empty() ? color.muted : color.amber);
    button(s, {s.width - 153, lower + 9, s.width - 16, lower + 45}, L"对比   空格", Command::Compare, 1);
}

View App::view(const Surface& s, const Pane& pane, size_t imageSlot) const {
    const auto& slot = slots_[imageSlot];
    const auto size = slot.orientation.size(slot.image->w, slot.image->h);
    const double scale = (actual_ ? 1. / s.dpi : fit(pane.area, size.x, size.y)) * zoom_;
    Point offset = slot.offset;
    if (imageSlot != pane.slot && slots_[pane.slot].image && slots_[pane.slot].image->display) {
        const auto& original = slots_[pane.slot];
        const auto source = original.orientation.size(original.image->w, original.image->h);
        offset = {original.offset.x * size.x / source.x, original.offset.y * size.y / source.y};
    }
    auto top = pane.area.center() + Point{(groupPan_.x - .5) * size.x + offset.x,
        (groupPan_.y - .5) * size.y + offset.y} * scale;
    top = top + Point{zoomShift_.x * pane.area.width(), zoomShift_.y * pane.area.height()};
    if (actual_ && zoom_ == 1) {
        top.x = std::round(top.x * s.dpi) / s.dpi;
        top.y = std::round(top.y * s.dpi) / s.dpi;
    }
    return {pane.area, top, size, scale};
}

void App::drawHistogram(Surface& s, const Image& image, Box b) {
    b = inset(b, 5);
    uint64_t maxCount{};
    for (const auto& channel : image.histogram)
        for (auto count : channel) maxCount = (std::max)(maxCount, count);
    if (!maxCount) return;
    line(s, {b.l, b.b}, {b.r, b.b}, color.line);
    const D2D1_COLOR_F inks[]{D2D1::ColorF(0xff6b75, .84f), D2D1::ColorF(0x77de99, .84f), D2D1::ColorF(0x6b9cff, .88f)};
    for (size_t c = 0; c < 3; ++c) {
        auto vertex = [&](size_t i) {
            return Point{b.l + b.width() * double(i) / 255,
                b.b - b.height() * double(image.histogram[c][i]) / double(maxCount)};
        };
        for (size_t i = 1; i < 256; ++i) line(s, vertex(i - 1), vertex(i), inks[c], .8f);
    }
}

void App::drawPane(Surface& s, const Pane& pane) {
    const Box area = pane.area;
    s.target->PushAxisAlignedClip(rect(area), D2D1_ANTIALIAS_MODE_ALIASED);
    const size_t source = visibleSlot(pane.slot);
    const bool overlay = source != pane.slot;
    const auto& shown = slots_[source];
    if (!shown.image || !shown.image->display) {
        label(s, !shown.image ? L"正在载入…" : shown.image->error,
            {area.l + 12, area.center().y - 20, area.r - 12, area.center().y + 20}, 14,
            shown.image ? color.pink : color.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        const auto layout = view(s, pane, source);
        const bool delta = diffOn_ && pane.slot == 1;
        const auto raster = delta ? diffRaster_ : shown.image->display;
        if (raster) {
            bitmap(s, raster, layout.top, layout.scale, true, shown.orientation);
        } else {
            const bool failed = slots_[0].image && !slots_[0].image->display;
            label(s, failed ? L"左图加载失败，无法计算差值" : L"正在计算差值…",
                {area.l + 12, area.center().y - 20, area.r - 12, area.center().y + 20}, 13,
                color.muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        const auto drawRoi = [&](Box box, bool draft) {
            const auto start = layout.screen({box.l, box.t}), end = layout.screen({box.r, box.b});
            const Box rim{start.x, start.y, end.x, end.y};
            const auto ink = draft ? color.fg : color.amber;
            outline(s, rim, D2D1::ColorF(0x0c1016, .9f), 0, 3);
            outline(s, rim, ink, 0, 1.2f);
            if (info_ && !draft) {
                label(s, L"ROI", {rim.l + 4, rim.t - 19, rim.l + 32, rim.t}, 11, ink,
                    DWRITE_TEXT_ALIGNMENT_LEADING, false, false, true);
            }
        };
        const bool editing = roiPreview_ && (!drag_.singleRoi || source == visibleSlot(drag_.slot));
        if (editing) drawRoi(roiPreview_->boxes[source], true);
        else if (roi_) drawRoi(roi_->boxes[source], false);

        if (probeEnabled_ && probe_) {
            const int x = int(std::floor(probe_->x * layout.size.x)), y = int(std::floor(probe_->y * layout.size.y));
            if (const auto* p = shown.image->oriented(x, y, shown.orientation)) {
                const auto cross = layout.screen({(x + .5) / layout.size.x, (y + .5) / layout.size.y});
                for (auto ink : std::array<D2D1_COLOR_F, 2>{D2D1::ColorF(0x11151c), color.blue}) {
                    const float stroke = ink.r == color.blue.r ? 1.f : 3.f;
                    line(s, {cross.x - 11, cross.y}, {cross.x + 11, cross.y}, ink, stroke);
                    line(s, {cross.x, cross.y - 11}, {cross.x, cross.y + 11}, ink, stroke);
                }
                if (info_) {
                    const auto n = [bits = shown.image->bits](uint16_t value) { return std::to_wstring(nativeValue(value, bits)); };
                    std::wstring reading = L"(" + std::to_wstring(x) + L", " + std::to_wstring(y) + L")  ";
                    if (delta && diffValues_) {
                        const auto native = shown.orientation.inverse({x + .5, y + .5}, shown.image->w, shown.image->h);
                        const auto value = (*diffValues_)[size_t(native.y) * shown.image->w + size_t(native.x)];
                        reading += std::isfinite(value) ? L"ΔY " + fixed(value, 3) : L"无重叠像素";
                    } else {
                        reading += shown.image->gray ? L"Y " + n(p->r) : L"RGB " + n(p->r) + L" / " + n(p->g) + L" / " + n(p->b);
                    }
                    const Box box{area.l + 7, area.t + 35, (std::min)(area.r - 7, area.l + 240), area.t + 58};
                    label(s, reading, inset(box, 4), 11, color.blue, DWRITE_TEXT_ALIGNMENT_LEADING,
                        false, false, true);
                }
            }
        }
        const bool showHistogram = info_ && histogram_ && area.width() >= 138 && area.height() >= 124 && !delta;
        const bool showStats = info_ && roi_ && !editing && area.height() >= 140 && !delta;
        if (showHistogram) {
            const double bottom = showStats && area.width() < 389 ? area.b - 118 : area.b - 7;
            drawHistogram(s, *shown.image, {area.r - 122, bottom - 51, area.r - 7, bottom});
        }

        if (showStats) {
            const Box stats{area.l + 7, area.b - 112, (std::min)(area.r - 7, area.l + 255), area.b - 7};
            if (!shown.measurement) {
                label(s, L"正在统计 ROI…", inset(stats, 7), 12, color.fg, DWRITE_TEXT_ALIGNMENT_LEADING,
                    false, false, true);
            } else {
                const auto& m = *shown.measurement;
                const auto tuple = [](const std::array<double, 3>& a) {
                    return fixed(a[0]) + L" / " + fixed(a[1]) + L" / " + fixed(a[2]);
                };
                const std::wstring hsv = (m.hueValid ? fixed(m.hsv[0]) + L"°" : L"—") +
                    L" / " + fixed(m.hsv[1]) + L"% / " + fixed(m.hsv[2]) + L"%";
                const auto text = L"ROI   " + std::to_wstring(m.x) + L"," +
                    std::to_wstring(m.y) + L"   " + std::to_wstring(m.w) + L"×" + std::to_wstring(m.h) +
                    L"   n=" + std::to_wstring(m.count) + L"\n" +
                    (m.count ? L"RGB   " + tuple(m.rgb) + L"\nLab    " + tuple(m.lab) + L"\nHSV   " + hsv +
                        L"\n空域 σY   " + (m.count > 1 ? fixed(m.sigma, 3) + L" DN" : L"—") : L"框内无有效像素");
                label(s, text, inset(stats, 7), 12, color.fg, DWRITE_TEXT_ALIGNMENT_LEADING, false, true, true);
            }
        }
        if (info_) {
            const Box scale{area.r - 84, area.t + 6, area.r - 7, area.t + 30};
            label(s, fixed(layout.scale * s.dpi * 100, layout.scale * s.dpi < 1 ? 1 : 0) + L"%",
                scale, 12, color.blue, DWRITE_TEXT_ALIGNMENT_CENTER, false, false, true);
        }
    }
    if (info_) {
        const bool delta = diffOn_ && pane.slot == 1;
        const Box name{area.l + 7, area.t + 6, (std::max)(area.l + 8, area.r - 88), area.t + 30};
        std::wstring title = displayPath(shown.path);
        if (overlay) title = L"覆盖 · " + title;
        if (delta) title = L"差值 ×" + fixed(gain_, 2) + L" · " + title;
        if (shown.image && shown.image->display)
            title += L"   " + std::to_wstring(shown.image->w) + L"×" + std::to_wstring(shown.image->h) +
                L" · " + std::to_wstring(shown.image->bits) + L" bit";
        label(s, title, {name.l + 6, name.t, name.r - 4, name.b}, 12,
            overlay || delta ? color.amber : color.fg, DWRITE_TEXT_ALIGNMENT_LEADING, false, false, true);
    }
    s.target->PopAxisAlignedClip();
    const double x = std::floor(area.r * s.dpi) / s.dpi - .5 / s.dpi;
    const double y = std::floor(area.b * s.dpi) / s.dpi - .5 / s.dpi;
    line(s, {x, area.t}, {x, area.b}, color.line, float(1 / s.dpi));
    line(s, {area.l, y}, {area.r, y}, color.line, float(1 / s.dpi));
}

void App::drawCompare(Surface& s) {
    const double bar = 49, bottom = s.height - 27;
    fill(s, {0, 0, s.width, bar}, color.panel);
    button(s, {12, 8, 93, 41}, L"‹ 浏览", Command::Back);
    button(s, {105, 8, 186, 41}, L"上一组 B", Command::Previous);
    button(s, {192, 8, 289, 41}, L"下一组 空格", Command::Next, 1);
    button(s, {305, 8, 367, 41}, L"100%", Command::Actual, actual_ ? 1 : 0);
    button(s, {373, 8, 441, 41}, L"复位 R", Command::Reset);
    button(s, {453, 8, 524, 41}, L"直方图 H", Command::Hist, histogram_ ? 1 : 0);
    button(s, {530, 8, 601, 41}, L"探针 P", Command::Probe, probeEnabled_ ? 1 : 0);
    if (split_ && slots_.size() > 2) {
        button(s, {613, 8, 641, 41}, L"−", Command::SplitFewer, 0, 0, 0, L"第一屏少一张");
        button(s, {646, 8, 674, 41}, L"＋", Command::SplitMore, 0, 0, 0, L"第一屏多一张");
    }
    button(s, {s.width - 171, 8, s.width - 97, 41}, split_ ? L"合并屏幕" : L"双屏 F", Command::Split);
    button(s, {s.width - 89, 8, s.width - 12, 41}, L"快捷键 K", Command::Help);
    fill(s, {0, bar, s.width, bottom}, color.canvas);
    const size_t boundary = split_ ? splitBoundary_ : slots_.size();
    const size_t begin = s.secondary ? boundary : 0, end = s.secondary ? slots_.size() : boundary;
    const size_t count = end - begin;
    if (count) {
        const size_t cols = count <= 3 ? count : (count + 1) / 2;
        const size_t rows = count <= 3 ? 1 : 2;
        const double w = s.width / cols, h = (bottom - bar) / rows;
        for (size_t i = 0; i < count; ++i) {
            const double x = (i % cols) * w, y = bar + (i / cols) * h;
            s.panes.push_back({begin + i, {x, y, x + w, y + h}});
            drawPane(s, s.panes.back());
        }
    }
    fill(s, {0, bottom, s.width, s.height}, color.panel);
    std::wstring text = status_.empty() ? L"S 框选 ROI   ·   Shift 拖框   ·   Ctrl + Shift 单图拖框" : status_;
    if (status_.empty() && diffOn_) text = L"右侧滚轮调差值增益   ·   Shift + 滚轮缩放   ·   D 退出差值";
    label(s, text, {12, bottom, s.width - 110, s.height}, 11, status_.empty() ? color.muted : color.amber);
    label(s, std::to_wstring(slots_.size()) + L" 张图" + (split_ ? (s.secondary ? L" · 第二屏" : L" · 第一屏") : L""),
        {s.width - 121, bottom, s.width - 13, s.height}, 11, color.muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void App::drawModal(Surface& s) {
    s.hits.clear();
    fill(s, {0, 0, s.width, s.height}, D2D1::ColorF(0x0a0f17, .75f));
    const auto center = Box{0, 0, s.width, s.height}.center();
    const double w = modal_ == Modal::Help ? 704 : 550;
    const double h = modal_ == Modal::Help ? 437 : modal_ == Modal::Range ? 372 : 234;
    const Box b{center.x - w / 2, center.y - h / 2, center.x + w / 2, center.y + h / 2};
    fill(s, b, color.panel, 16);
    outline(s, b, color.line, 16);
    std::wstring title;
    switch (modal_) {
    case Modal::Help: title = L"快捷键"; break;
    case Modal::Delete: title = L"移除 " + std::to_wstring(pendingDelete_.size()) + L" 张图片"; break;
    case Modal::Permanent: title = L"永久删除 " + std::to_wstring(pendingDelete_.size()) + L" 个文件？"; break;
    case Modal::TooMany: title = L"已选 " + std::to_wstring(pendingSelection_.size()) + L" 张图片"; break;
    case Modal::Range: title = L"选择本次对比范围"; break;
    default: return;
    }
    label(s, title, {b.l + 23, b.t + 13, b.r - 23, b.t + 63}, 22,
        modal_ == Modal::Permanent ? color.pink : color.fg, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    line(s, {b.l + 23, b.t + 66}, {b.r - 23, b.t + 66}, color.line);
    const double y = b.b - 56;
    if (modal_ == Modal::Help) {
        const std::pair<const wchar_t*, const wchar_t*> left[]{
            {L"空格 / B", L"开始对比或下一组 / 上一组"},
            {L"左键 / Ctrl + 左键拖", L"联动平移 / 单图平移"},
            {L"滚轮 / 1", L"联动缩放 / 100%"},
            {L"按住 S + 左键拖", L"框选 ROI，替换旧框"},
            {L"Shift + 左键拖框", L"框内移动 / 边角调整大小"},
            {L"ROI 右键", L"删除该 ROI"},
            {L"O / I / U", L"旋转 90° / 镜像 / 翻转"},
            {L"Ctrl + O / I / U", L"变换当前窗格"}
        };
        const std::pair<const wchar_t*, const wchar_t*> right[]{
            {L"Ctrl + Shift + 左键拖框", L"只调整当前图的 ROI"},
            {L"按住 C", L"右图覆盖左图（双图）"},
            {L"D", L"开关右图差值（双图）"},
            {L"差值图滚轮", L"调差值增益，按 Shift 缩放"},
            {L"H / P / A", L"直方图 / 探针 / 叠加信息"},
            {L"F / R", L"双屏 / 复位视图"},
            {L"Delete", L"预览页移除所选图片"},
            {L"Shift + 滚轮", L"预览页横向滚动"},
            {L"K / Esc", L"关闭帮助 / 返回浏览"}
        };
        const auto list = [&](const auto& items, double x) {
            double top = b.t + 78;
            for (const auto& [key, meaning] : items) {
                label(s, key, {x, top, x + 148, top + 33}, 12, color.blue);
                label(s, meaning, {x + 148, top, x + 319, top + 33}, 12);
                top += 34;
            }
        };
        list(left, b.l + 23);
        list(right, b.l + 363);
        button(s, {b.r - 105, y, b.r - 23, y + 36}, L"关闭", Command::Cancel, 1);
    } else if (modal_ == Modal::Delete || modal_ == Modal::Permanent) {
        const auto names = pendingDelete_.size() == 1 ? pendingDelete_[0].wstring() :
            pendingDelete_[0].wstring() + L"\n及另外 " + std::to_wstring(pendingDelete_.size() - 1) + L" 个文件";
        label(s, names, {b.l + 24, b.t + 78, b.r - 24, b.t + 126}, 12, color.fg,
            DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
        label(s, modal_ == Modal::Permanent ? L"文件将从磁盘删除，不经过回收站。" : L"移出预览后，重新载入目录即可再次看到。",
            {b.l + 24, b.t + 128, b.r - 24, b.t + 166}, 13,
            modal_ == Modal::Permanent ? color.pink : color.muted);
        button(s, {b.r - 95, y, b.r - 23, y + 36}, L"取消", Command::Cancel);
        if (modal_ == Modal::Delete) {
            button(s, {b.r - 369, y, b.r - 235, y + 36}, L"移出本次预览", Command::Remove, 1);
            button(s, {b.r - 227, y, b.r - 103, y + 36}, L"永久删除文件", Command::ConfirmDelete, 2);
        } else {
            button(s, {b.r - 228, y, b.r - 103, y + 36}, L"确认永久删除", Command::ConfirmDelete, 2);
        }
        button(s, {b.l + 23, y, b.l + 108, y + 36}, L"复制路径", Command::CopyPath);
    } else if (modal_ == Modal::TooMany) {
        label(s, L"一次最多对比 8 张。可以调整预览选择，或从已选图片中选一段。",
            {b.l + 24, b.t + 81, b.r - 24, b.t + 142}, 15, color.muted,
            DWRITE_TEXT_ALIGNMENT_LEADING, false, true);
        button(s, {b.r - 247, y, b.r - 149, y + 36}, L"返回调整", Command::Cancel);
        button(s, {b.r - 140, y, b.r - 23, y + 36}, L"选择对比范围", Command::Range, 1);
    } else if (modal_ == Modal::Range) {
        const size_t end = (std::min)(pendingSelection_.size(), rangeStart_ + maxCompare);
        label(s, L"第 " + std::to_wstring(rangeStart_ + 1) + L"–" + std::to_wstring(end) + L" 张  /  共 " +
            std::to_wstring(pendingSelection_.size()) + L" 张", {b.l + 24, b.t + 73, b.r - 24, b.t + 103}, 14, color.blue);
        for (size_t i = rangeStart_; i < end; ++i) {
            const auto [col, row] = pendingSelection_[i];
            const double top = b.t + 104 + (i - rangeStart_) * 23;
            label(s, std::to_wstring(i + 1) + L"    " + displayPath(columns_[col].files[row].path),
                {b.l + 25, top, b.r - 25, top + 23}, 13);
        }
        button(s, {b.l + 23, y, b.l + 102, y + 36}, L"往前一张", Command::RangePrev);
        button(s, {b.l + 110, y, b.l + 189, y + 36}, L"往后一张", Command::RangeNext);
        button(s, {b.r - 217, y, b.r - 132, y + 36}, L"返回调整", Command::Cancel);
        button(s, {b.r - 123, y, b.r - 23, y + 36}, L"开始对比", Command::RangeOpen, 1);
    }
}

void App::drawContext(Surface& s) {
    if (!context_ || s.secondary) return;
    const auto& hit = *context_;
    const double x = std::clamp(contextPoint_.x, 6., s.width - 181);
    const double y = std::clamp(contextPoint_.y, 6., s.height - 46);
    s.hits.clear();
    fill(s, {x, y, x + 175, y + 40}, color.panel, 8);
    outline(s, {x, y, x + 175, y + 40}, color.line, 8);
    button(s, {x + 4, y + 4, x + 171, y + 36},
        hit.action == Command::Remove ? L"移除或删除图片…" : L"添加此文件夹",
        hit.action, 0, hit.a, hit.b);
}

void App::tooltip(Surface& s) {
    if (modal_ != Modal::None || context_ || drag_.mode != Gesture::None) return;
    if (!tooltipReady_) return;
    for (auto it = s.hits.rbegin(); it != s.hits.rend(); ++it) {
        if (it->tip.empty() || !it->box.contains(s.mouse)) continue;
        const auto width = std::clamp(it->tip.size() * 8. + 20, 112., s.width - 30);
        const auto x = std::clamp(s.mouse.x + 12, 8., s.width - width - 8);
        const auto y = (std::min)(s.mouse.y + 19, s.height - 33);
        const Box b{x, y, x + width, y + 26};
        fill(s, b, color.bg, 5);
        outline(s, b, color.line, 5);
        label(s, it->tip, inset(b, 5), 12, color.fg);
        break;
    }
}

void App::paint(Surface& s) {
    if (!target(s)) return;
    for (auto it = s.bitmaps.begin(); it != s.bitmaps.end();) {
        if (it->second.empty() || it->second.front().source.expired()) it = s.bitmaps.erase(it);
        else ++it;
    }
    const auto dim = s.target->GetSize();
    s.width = dim.width;
    s.height = dim.height;
    s.hits.clear();
    s.panes.clear();
    s.grid = {};
    s.target->BeginDraw();
    s.target->Clear(color.bg);
    if (comparing_) drawCompare(s);
    else drawBrowser(s);
    if (modal_ != Modal::None) {
        if (s.hwnd == modalWindow_) drawModal(s);
        else {
            s.hits.clear();
            fill(s, {0, 0, s.width, s.height}, D2D1::ColorF(0x0a0f17, .48f));
        }
    } else if (context_) drawContext(s);
    else tooltip(s);
    const HRESULT result = s.target->EndDraw();
    if (FAILED(result)) discardTarget(s);
    if (result == D2DERR_RECREATE_TARGET) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void App::setModal(Modal modal) {
    modal_ = modal;
    context_.reset();
    keyboardFocus_ = modal != Modal::None;
    focusIndex_ = 0;
    cHeld_ = sHeld_ = false;
    clearDrag();
    invalidate();
}

void App::chooseDelete(std::vector<fs::path> paths) {
    if (paths.empty()) {
        status(L"请先选中要移除的图片。");
        return;
    }
    pendingDelete_ = std::move(paths);
    modalWindow_ = main_.hwnd;
    setModal(Modal::Delete);
}

void App::removeFiles(bool permanent) {
    size_t count{}, failed{};
    for (const auto& path : pendingDelete_) {
        if (permanent && !DeleteFileW(path.c_str())) {
            ++failed;
            continue;
        }
        for (auto& col : columns_) {
            if (col.path != path.parent_path()) continue;
            if (!permanent) col.hidden.insert(path);
            std::erase_if(col.files, [&](const Entry& e) { return e.path == path; });
        }
        selection_.erase(path);
        ++count;
    }
    pendingDelete_.clear();
    setModal(Modal::None);
    auto text = std::wstring(permanent ? L"已永久删除 " : L"已移出预览 ") + std::to_wstring(count) + L" 张。";
    if (failed) text += L" 有 " + std::to_wstring(failed) + L" 个文件无法删除。";
    status(text);
}

static bool copyText(HWND window, const std::wstring& text) {
    auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory) return false;
    if (auto* buffer = GlobalLock(memory)) {
        memcpy(buffer, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);
    } else {
        GlobalFree(memory);
        return false;
    }
    bool written{};
    if (OpenClipboard(window)) {
        EmptyClipboard();
        written = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        CloseClipboard();
    }
    if (!written) GlobalFree(memory);
    return written;
}

void App::command(const Hit& hit, bool ctrl) {
    const auto action = hit.action;
    const size_t a = hit.a, b = hit.b;
    const auto keepContext = context_;
    context_.reset();
    switch (action) {
    case Command::Add: pickFolder(); break;
    case Command::SortName:
    case Command::SortTime:
    case Command::Order:
        if (action == Command::Order) descending_ = !descending_;
        else byTime_ = action == Command::SortTime;
        for (auto& col : columns_) {
            col.manualOrder = false;
            sort(col);
        }
        status_.clear();
        break;
    case Command::Compare: modalWindow_ = main_.hwnd; beginCompare(); break;
    case Command::Back: back(); break;
    case Command::Previous: step(-1); break;
    case Command::Next: step(1); break;
    case Command::Split: split(); break;
    case Command::SplitMore: if (split_ && splitBoundary_ + 1 < slots_.size()) ++splitBoundary_; break;
    case Command::SplitFewer: if (split_ && splitBoundary_ > 1) --splitBoundary_; break;
    case Command::Help: setModal(Modal::Help); break;
    case Command::ToggleTree: treeVisible_ = !treeVisible_; break;
    case Command::Refresh: refresh(); break;
    case Command::ColumnSelect: {
        std::vector<fs::path> paths;
        for (const auto& entry : columns_[a].files) paths.push_back(entry.path);
        select(paths, ctrl);
        break;
    }
    case Command::ColumnClose:
        for (const auto& entry : columns_[a].files) selection_.erase(entry.path);
        columns_.erase(columns_.begin() + a);
        break;
    case Command::ColumnPath:
        currentFolder_ = columns_[a].path;
        showInTree(currentFolder_);
        status(currentFolder_.wstring());
        break;
    case Command::RowSelect: {
        std::vector<fs::path> paths;
        for (const auto& col : columns_) if (a < col.files.size()) paths.push_back(col.files[a].path);
        select(paths, ctrl);
        break;
    }
    case Command::FileSelect: select({columns_[a].files[b].path}, ctrl); break;
    case Command::TreeOpen: currentFolder_ = treePaths_[a]; break;
    case Command::TreeToggle: {
        currentFolder_ = treePaths_[a];
        if (auto* node = findTree(currentFolder_, tree_)) {
            if (node->expanded) node->expanded = false;
            else expand(*node);
        }
        break;
    }
    case Command::TreeAdd:
        if (keepContext && !keepContext->tip.empty()) addFolder(keepContext->tip);
        else if (a < treePaths_.size()) addFolder(treePaths_[a]);
        break;
    case Command::Remove:
        if (modal_ == Modal::Delete) removeFiles(false);
        else if (keepContext && !keepContext->tip.empty()) {
            const fs::path path = keepContext->tip;
            if (selection_.contains(path)) chooseDelete({selection_.begin(), selection_.end()});
            else chooseDelete({path});
        }
        break;
    case Command::ConfirmDelete:
        if (modal_ == Modal::Permanent) removeFiles(true);
        else setModal(Modal::Permanent);
        break;
    case Command::Cancel: setModal(Modal::None); break;
    case Command::Range: setModal(Modal::Range); break;
    case Command::RangePrev: if (rangeStart_) --rangeStart_; break;
    case Command::RangeNext: if (rangeStart_ + maxCompare < pendingSelection_.size()) ++rangeStart_; break;
    case Command::RangeOpen: {
        const auto end = (std::min)(pendingSelection_.size(), rangeStart_ + maxCompare);
        openCompare({pendingSelection_.begin() + rangeStart_, pendingSelection_.begin() + end});
        break;
    }
    case Command::CopyPath: {
        std::wstring paths;
        for (const auto& path : pendingDelete_) {
            if (!paths.empty()) paths += L"\r\n";
            paths += path.wstring();
        }
        status(copyText(main_.hwnd, paths) ? L"已复制路径。" : L"无法复制路径。");
        break;
    }
    case Command::Hist: histogram_ = !histogram_; break;
    case Command::Probe: probeEnabled_ = !probeEnabled_; break;
    case Command::Info: info_ = !info_; break;
    case Command::Reset: reset(); break;
    case Command::Actual:
        zoomShift_ = zoomShift_ * (1 / zoom_);
        zoom_ = 1;
        actual_ = true;
        break;
    }
    invalidate();
}

std::optional<Pane> App::paneAt(Surface& s, Point p) const {
    for (const auto& pane : s.panes) if (pane.area.contains(p)) return pane;
    return {};
}

size_t App::visibleSlot(size_t paneSlot) const {
    return cHeld_ && paneSlot == 0 && slots_.size() == 2 && slots_[1].image &&
        slots_[1].image->display ? 1 : paneSlot;
}

Point App::normal(const Surface& s, const Pane& pane, Point p) const {
    return view(s, pane, visibleSlot(pane.slot)).normal(p);
}

bool App::roiAt(const Surface& s, const Pane& pane, Point p, unsigned& edges) const {
    if (!roi_) return false;
    const size_t source = visibleSlot(pane.slot);
    const auto layout = view(s, pane, source);
    const auto box = roi_->boxes[source];
    const auto begin = layout.screen({box.l, box.t}), end = layout.screen({box.r, box.b});
    if (!Box{begin.x - 6, begin.y - 6, end.x + 6, end.y + 6}.contains(p)) return false;
    edges = 0;
    if ((std::min)(std::abs(p.x - begin.x), std::abs(p.x - end.x)) <= 6)
        edges |= std::abs(p.x - begin.x) <= std::abs(p.x - end.x) ? RoiLeft : RoiRight;
    if ((std::min)(std::abs(p.y - begin.y), std::abs(p.y - end.y)) <= 6)
        edges |= std::abs(p.y - begin.y) <= std::abs(p.y - end.y) ? RoiTop : RoiBottom;
    return true;
}

void App::clearDrag() {
    if (drag_.reorder) KillTimer(drag_.hwnd, reorderScrollTimer);
    drag_ = {};
    roiPreview_.reset();
    if (GetCapture() == main_.hwnd || (second_.hwnd && GetCapture() == second_.hwnd)) ReleaseCapture();
}

std::optional<ReorderDrop> App::reorderDrop(const Surface& s, Point p) const {
    const auto& grid = s.grid;
    if (!drag_.moved || !drag_.reorder || !grid.area.contains(p)) return {};
    const auto& origin = *drag_.reorder;
    const double x = grid.area.l + origin.a * grid.columnWidth - scrollX_;
    if (drag_.mode == Gesture::ColumnDrag) {
        const double column = (p.x - grid.area.l + scrollX_) / grid.columnWidth;
        const auto boundary = (std::min)(columns_.size(), size_t(std::round(column)));
        const double edge = std::clamp(grid.area.l + boundary * grid.columnWidth - scrollX_,
            grid.area.l + 2, grid.area.r - 2);
        return ReorderDrop{boundary, {edge - 1, grid.area.t + 3, edge + 1, grid.area.b}};
    }
    if (p.y < grid.bodyTop || p.x < x || p.x >= x + grid.columnWidth) return {};
    const double row = (p.y - grid.bodyTop + scrollY_) / rowHeight;
    const auto boundary = (std::min)(columns_[origin.a].files.size(), size_t(std::round(row)));
    const double edge = std::clamp(grid.bodyTop + boundary * rowHeight - scrollY_,
        grid.bodyTop + 1, grid.area.b - 1);
    return ReorderDrop{boundary, {x + 8, edge - 1, x + grid.columnWidth - 8, edge + 1}};
}

void App::beginPointer(Surface& s, Point p, UINT message, WPARAM keys) {
    if (drag_.mode != Gesture::None) return;
    const bool ctrl = (keys & MK_CONTROL) != 0;
    SetFocus(s.hwnd);
    s.mouse = p;
    keyboardFocus_ = false;
    tooltipReady_ = false;
    if (modal_ != Modal::None && s.hwnd != modalWindow_) return;
    if (message == WM_LBUTTONDOWN) {
        for (auto it = s.hits.rbegin(); it != s.hits.rend(); ++it) {
            if (!it->box.contains(p)) continue;
            const Hit hit = *it;
            if (hit.action == Command::ColumnPath || hit.action == Command::FileSelect) {
                drag_.mode = hit.action == Command::ColumnPath ? Gesture::ColumnDrag : Gesture::FileDrag;
                drag_.hwnd = s.hwnd;
                drag_.start = p;
                drag_.reorder = hit;
                drag_.ctrl = ctrl;
                SetCapture(s.hwnd);
                invalidate();
                return;
            }
            if (hit.action == Command::TreeOpen) {
                drag_.mode = Gesture::TreeDrag;
                drag_.hwnd = s.hwnd;
                drag_.start = p;
                drag_.folder = treePaths_[hit.a];
                SetCapture(s.hwnd);
                currentFolder_ = drag_.folder;
                invalidate();
                return;
            }
            if (hit.action == Command::Help) modalWindow_ = s.hwnd;
            command(hit, ctrl);
            return;
        }
    }
    if (modal_ != Modal::None) return;
    if (context_) {
        context_.reset();
        invalidate();
        return;
    }
    if (!comparing_) {
        if (message == WM_RBUTTONDOWN) {
            for (auto it = s.hits.rbegin(); it != s.hits.rend(); ++it) {
                if (!it->box.contains(p)) continue;
                if (it->action == Command::FileSelect) {
                    context_ = Hit{{}, Command::Remove, it->a, it->b, columns_[it->a].files[it->b].path.wstring()};
                } else if (it->action == Command::TreeOpen || it->action == Command::TreeToggle) {
                    context_ = Hit{{}, Command::TreeAdd, it->a, 0, treePaths_[it->a].wstring()};
                }
                if (context_) {
                    contextPoint_ = p;
                    invalidate();
                }
                break;
            }
        }
        return;
    }
    const auto pane = paneAt(s, p);
    if (!pane) return;
    hotSlot_ = pane->slot;
    const auto& slot = slots_[hotSlot_];
    if (!slot.image || !slot.image->display) return;
    const auto layout = view(s, *pane, visibleSlot(hotSlot_));
    unsigned edges{};
    const bool overRoi = roiAt(s, *pane, p, edges);
    if (message == WM_RBUTTONDOWN) {
        if (overRoi) {
            roi_.reset();
            recompute();
            invalidate();
        }
        return;
    }
    const bool shift = (keys & MK_SHIFT) != 0;
    if (shift && !overRoi && !sHeld_) return;
    const auto start = layout.normal(p);
    if (sHeld_ && !Box{0, 0, 1, 1}.contains(start)) return;
    drag_.hwnd = s.hwnd;
    drag_.slot = hotSlot_;
    drag_.previous = p;
    drag_.start = start;
    if (shift && overRoi && !sHeld_) {
        drag_.mode = Gesture::EditRoi;
        drag_.edges = edges;
        drag_.singleRoi = ctrl;
        roiPreview_ = roi_;
    } else if (sHeld_) {
        drag_.mode = Gesture::NewRoi;
        const double x = std::floor(start.x * layout.size.x), y = std::floor(start.y * layout.size.y);
        roiPreview_.emplace(Box{x / layout.size.x, y / layout.size.y, (x + 1) / layout.size.x, (y + 1) / layout.size.y});
    } else {
        drag_.mode = ctrl ? Gesture::Single : Gesture::Group;
    }
    SetCapture(s.hwnd);
    invalidate();
}

void App::movePointer(Surface& s, Point p) {
    s.mouse = p;
    tooltipReady_ = false;
    KillTimer(s.hwnd, 2);
    SetTimer(s.hwnd, 2, 640, nullptr);
    if (drag_.reorder && !drag_.moved && drag_.hwnd == s.hwnd) {
        const auto delta = p - drag_.start;
        if (std::hypot(delta.x, delta.y) >= 6) {
            drag_.moved = true;
            SetTimer(s.hwnd, reorderScrollTimer, 40, nullptr);
        }
    }
    if (modal_ != Modal::None || !comparing_ || drag_.mode == Gesture::TreeDrag) {
        invalidate();
        return;
    }
    auto pane = paneAt(s, p);
    if (drag_.hwnd == s.hwnd) {
        for (const auto& candidate : s.panes) if (candidate.slot == drag_.slot) pane = candidate;
    }
    probe_.reset();
    if (!pane || !slots_[pane->slot].image || !slots_[pane->slot].image->display) {
        invalidate();
        return;
    }
    hotSlot_ = pane->slot;
    auto& slot = slots_[hotSlot_];
    const auto layout = view(s, *pane, visibleSlot(hotSlot_));
    if (drag_.hwnd == s.hwnd) {
        const auto movement = (p - drag_.previous) * (1. / layout.scale);
        const auto coord = layout.normal(p);
        const auto step = coord - drag_.start;
        switch (drag_.mode) {
        case Gesture::Group:
            groupPan_ = groupPan_ + Point{movement.x / layout.size.x, movement.y / layout.size.y};
            break;
        case Gesture::Single: {
            const auto size = slot.orientation.size(slot.image->w, slot.image->h);
            slot.offset = slot.offset + Point{movement.x * size.x / layout.size.x, movement.y * size.y / layout.size.y};
            break;
        }
        case Gesture::NewRoi: {
            const auto cell = [](double v, double size) { return std::clamp(std::floor(v * size), 0., size - 1); };
            const double x0 = cell(drag_.start.x, layout.size.x), x1 = cell(coord.x, layout.size.x);
            const double y0 = cell(drag_.start.y, layout.size.y), y1 = cell(coord.y, layout.size.y);
            roiPreview_.emplace(Box{(std::min)(x0, x1) / layout.size.x, (std::min)(y0, y1) / layout.size.y,
                ((std::max)(x0, x1) + 1) / layout.size.x, ((std::max)(y0, y1) + 1) / layout.size.y});
            break;
        }
        case Gesture::EditRoi: {
            std::array<Point, maxCompare> pixelSizes;
            pixelSizes.fill({1 / layout.size.x, 1 / layout.size.y});
            for (size_t i = 0; i < slots_.size(); ++i) {
                const auto& item = slots_[i];
                if (!item.image || !item.image->display) continue;
                const auto size = item.orientation.size(item.image->w, item.image->h);
                pixelSizes[i] = {1 / size.x, 1 / size.y};
            }
            roiPreview_ = editRoi(*roi_, visibleSlot(hotSlot_), slots_.size(), drag_.singleRoi,
                step, pixelSizes, drag_.edges);
            break;
        }
        default: break;
        }
        drag_.previous = p;
    } else if (probeEnabled_) {
        const auto coord = normal(s, *pane, p);
        if (Box{0, 0, 1, 1}.contains(coord)) probe_ = coord;
    }
    invalidate();
}

void App::endPointer(Surface& s, Point p) {
    if (drag_.hwnd != s.hwnd) return;
    if (drag_.reorder) {
        movePointer(s, p);
        const auto hit = *drag_.reorder;
        const bool ctrl = drag_.ctrl, moved = drag_.moved;
        const auto drop = reorderDrop(s, p);
        clearDrag();
        if (!moved && hit.box.contains(p)) command(hit, ctrl);
        if (drop) {
            if (hit.action == Command::ColumnPath) {
                if (moveBefore(columns_, hit.a, drop->boundary)) status(L"已调整文件夹列顺序。");
            } else {
                auto& col = columns_[hit.a];
                if (moveBefore(col.files, hit.b, drop->boundary)) {
                    col.manualOrder = true;
                    status(L"已调整图片顺序，对比翻页将按此顺序进行。");
                }
            }
        }
        invalidate();
        return;
    }
    if (drag_.mode == Gesture::TreeDrag) {
        const auto folder = drag_.folder;
        const auto delta = p - drag_.start;
        clearDrag();
        if (std::hypot(delta.x, delta.y) > 6) {
            for (const auto& hit : s.hits) if (hit.action == Command::Add && hit.box.contains(p)) {
                addFolder(folder);
                break;
            }
        }
        invalidate();
        return;
    }
    movePointer(s, p);
    if (roiPreview_) {
        roi_ = roiPreview_;
        for (size_t i = 0; i < slots_.size(); ++i)
            if (!drag_.singleRoi || i == visibleSlot(drag_.slot)) slots_[i].measurement.reset();
        recompute();
    }
    clearDrag();
    invalidate();
}

void App::wheel(Surface& s, Point p, double steps, bool horizontal) {
    if (modal_ != Modal::None || context_ || drag_.mode != Gesture::None) return;
    if (!comparing_) {
        if (horizontal || held(VK_SHIFT)) scrollX_ = (std::max)(0., scrollX_ + steps * (horizontal ? 130 : -130));
        else if (treeVisible_ && p.x < 216) treeY_ = std::clamp(treeY_ - steps * 72, 0.,
            (std::max)(0., treeRows_ * 30 - (s.height - footer - toolbar - 51)));
        else scrollY_ = (std::max)(0., scrollY_ - steps * rowHeight * 2);
        invalidate();
        return;
    }
    const auto pane = paneAt(s, p);
    if (!pane || !slots_[pane->slot].image || !slots_[pane->slot].image->display) return;
    if (diffOn_ && pane->slot == 1 && !held(VK_SHIFT)) {
        gain_ = std::clamp(gain_ * std::pow(1.3, steps), 1. / 32, 512.);
        updateDifference(false);
    } else {
        const size_t source = visibleSlot(pane->slot);
        const auto before = view(s, *pane, source);
        const auto previous = zoom_;
        zoom_ = std::clamp(zoom_ * std::pow(1.22, steps), .015, (std::max)(512., 64 * zoom_ / (before.scale * s.dpi)));
        zoomShift_ = zoomTranslation(zoomShift_, paneFraction(pane->area, p), zoom_ / previous);
    }
    invalidate();
}

void App::key(Surface& s, WPARAM code, LPARAM modifiers, bool down) {
    const auto k = static_cast<wchar_t>(code);
    if (!down) {
        if (k == L'S') sHeld_ = false;
        if (k == L'C' && cHeld_) {
            if (drag_.mode != Gesture::None) clearDrag();
            cHeld_ = false;
            invalidate();
        }
        return;
    }
    if (modal_ != Modal::None) {
        if (k == VK_ESCAPE || (k == L'K' && modal_ == Modal::Help)) {
            setModal(Modal::None);
        } else if (s.hwnd == modalWindow_) {
            if (k == VK_TAB && !s.hits.empty()) {
                focusIndex_ = (focusIndex_ + s.hits.size() + (held(VK_SHIFT) ? -1 : 1)) % s.hits.size();
                keyboardFocus_ = true;
                invalidate();
            } else if (k == VK_RETURN && !(modifiers & (1ll << 30))) {
                if (modal_ == Modal::Permanent) setModal(Modal::None);
                else if (focusIndex_ < s.hits.size() && s.hits[focusIndex_].action != Command::ConfirmDelete) {
                    const auto hit = s.hits[focusIndex_];
                    command(hit);
                }
            } else if (held(VK_CONTROL) && k == L'C' && (modal_ == Modal::Delete || modal_ == Modal::Permanent)) {
                command({{}, Command::CopyPath, 0, 0, {}});
            }
        }
        return;
    }
    if (context_) {
        if (k == VK_ESCAPE) { context_.reset(); invalidate(); }
        return;
    }
    if (drag_.mode != Gesture::None && k != L'S') {
        if (k == VK_ESCAPE && drag_.reorder) {
            clearDrag();
            invalidate();
        }
        return;
    }
    if ((modifiers & (1ll << 30)) && k != VK_TAB) return;
    const auto action = [&](Command c) { command({{}, c, 0, 0, {}}); };
    if (k == L'K') { modalWindow_ = s.hwnd; action(Command::Help); return; }
    if (k == VK_TAB && !s.hits.empty()) {
        if (!keyboardFocus_) focusIndex_ = held(VK_SHIFT) ? s.hits.size() - 1 : 0;
        else focusIndex_ = (focusIndex_ + s.hits.size() + (held(VK_SHIFT) ? -1 : 1)) % s.hits.size();
        keyboardFocus_ = true;
        invalidate();
        return;
    }
    if (k == VK_RETURN && keyboardFocus_ && focusIndex_ < s.hits.size()) {
        const auto hit = s.hits[focusIndex_];
        command(hit, held(VK_CONTROL));
        return;
    }
    if (!comparing_) {
        if (k == VK_SPACE) action(Command::Compare);
        if (k == VK_DELETE) chooseDelete({selection_.begin(), selection_.end()});
        if (k == VK_F5) action(Command::Refresh);
        return;
    }
    switch (k) {
    case VK_ESCAPE: action(Command::Back); break;
    case VK_SPACE: action(Command::Next); break;
    case L'B': action(Command::Previous); break;
    case L'H': action(Command::Hist); break;
    case L'A': action(Command::Info); break;
    case L'P': action(Command::Probe); break;
    case L'R': action(Command::Reset); break;
    case L'1': case VK_NUMPAD1: action(Command::Actual); break;
    case L'F': action(Command::Split); break;
    case L'S': sHeld_ = true; break;
    case L'C':
        if (slots_.size() == 2) {
            cHeld_ = true;
            invalidate();
        }
        break;
    case L'D':
        if (slots_.size() == 2) {
            diffOn_ = !diffOn_;
            updateDifference(diffOn_);
            invalidate();
        }
        break;
    case L'O': case L'I': case L'U': {
        const bool ctrl = held(VK_CONTROL);
        for (size_t i = 0; i < slots_.size(); ++i)
            if (!ctrl || i == hotSlot_) slots_[i].orientation.apply(k);
        for (auto& slot : slots_) slot.measurement.reset();
        recompute();
        updateDifference(true);
        invalidate();
        break;
    }
    }
}

LRESULT CALLBACK App::proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        const bool main = !app->main_.hwnd;
        auto& s = main ? app->main_ : app->second_;
        s.hwnd = hwnd;
        s.dpi = (std::max)(96u, GetDpiForWindow(hwnd)) / 96.;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        return DefWindowProcW(hwnd, message, w, l);
    }
    if (!app) return DefWindowProcW(hwnd, message, w, l);
    auto& s = hwnd == app->main_.hwnd ? app->main_ : app->second_;
    return app->message(s, hwnd, message, w, l);
}

LRESULT App::message(Surface& s, HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    const auto pointer = [&] { return Point{GET_X_LPARAM(l) / s.dpi, GET_Y_LPARAM(l) / s.dpi}; };
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* m = reinterpret_cast<MINMAXINFO*>(l);
        m->ptMinTrackSize.x = (std::max)(m->ptMinTrackSize.x, LONG(870 * s.dpi));
        m->ptMinTrackSize.y = (std::max)(m->ptMinTrackSize.y, LONG(535 * s.dpi));
        return 0;
    }
    case WM_DPICHANGED: {
        s.dpi = HIWORD(w) / 96.;
        const auto& box = *reinterpret_cast<const RECT*>(l);
        SetWindowPos(hwnd, nullptr, box.left, box.top, box.right - box.left, box.bottom - box.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        invalidate();
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        paint(s);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case workReady: drain(); return 0;
    case WM_TIMER:
        if (w == 1) { KillTimer(hwnd, 1); status_.clear(); }
        if (w == 2) { KillTimer(hwnd, 2); tooltipReady_ = true; }
        if (w == reorderScrollTimer) {
            const auto& grid = s.grid;
            const auto p = s.mouse;
            if (!reorderDrop(s, p)) return 0;
            if (drag_.mode == Gesture::ColumnDrag) {
                if (p.x < grid.area.l + 24) scrollX_ -= 20;
                else if (p.x >= grid.area.r - 24) scrollX_ += 20;
                else return 0;
            } else {
                if (p.y < grid.bodyTop + 24) scrollY_ -= 18;
                else if (p.y >= grid.area.b - 24) scrollY_ += 18;
                else return 0;
            }
            scrollX_ = (std::max)(0., scrollX_);
            scrollY_ = (std::max)(0., scrollY_);
        }
        invalidate();
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        beginPointer(s, pointer(), message, w); return 0;
    case WM_LBUTTONUP:
        endPointer(s, pointer()); return 0;
    case WM_LBUTTONDBLCLK:
        if (comparing_ && modal_ == Modal::None) {
            beginPointer(s, pointer(), WM_LBUTTONDOWN, w);
            return 0;
        }
        modalWindow_ = hwnd;
        if (!comparing_ && modal_ == Modal::None) {
            for (const auto& hit : s.hits) {
                if (!hit.box.contains(pointer())) continue;
                if (hit.action == Command::TreeOpen) {
                    addFolder(treePaths_[hit.a]);
                } else if (hit.action == Command::FileSelect) {
                    if (!selection_.contains(columns_[hit.a].files[hit.b].path)) command(hit);
                    beginCompare();
                }
                break;
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        movePointer(s, pointer());
        if (drag_.mode == Gesture::TreeDrag) SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
        else if (drag_.reorder && drag_.moved) {
            const auto cursor = drag_.mode == Gesture::ColumnDrag ? IDC_SIZEWE : IDC_SIZENS;
            SetCursor(LoadCursor(nullptr, reorderDrop(s, s.mouse) ? cursor : IDC_NO));
        }
        else if (comparing_ && sHeld_) SetCursor(LoadCursor(nullptr, IDC_CROSS));
        else SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        s.mouse = {-1, -1};
        KillTimer(hwnd, 2);
        tooltipReady_ = false;
        if (drag_.mode == Gesture::None) { probe_.reset(); invalidate(); }
        return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(hwnd, &p);
        wheel(s, {p.x / s.dpi, p.y / s.dpi}, GET_WHEEL_DELTA_WPARAM(w) / double(WHEEL_DELTA),
            message == WM_MOUSEHWHEEL);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (drag_.hwnd == hwnd) {
            if (drag_.reorder) KillTimer(hwnd, reorderScrollTimer);
            drag_ = {};
            roiPreview_.reset();
            invalidate();
        }
        return 0;
    case WM_KEYDOWN: case WM_KEYUP:
        key(s, w, l, message == WM_KEYDOWN); return 0;
    case WM_KILLFOCUS:
        cHeld_ = sHeld_ = false;
        invalidate();
        return 0;
    case WM_DISPLAYCHANGE:
        if (split_ && GetSystemMetrics(SM_CMONITORS) < 2) closeSecond();
        invalidate();
        return 0;
    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(w);
        if (!comparing_ && modal_ == Modal::None && !drag_.reorder) {
            UINT added{};
            const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                if (!DragQueryFileW(drop, i, path.data(), UINT(path.size()))) continue;
                fs::path folder(path.c_str());
                std::error_code ec;
                if (fs::is_directory(folder, ec)) { addFolder(folder); ++added; }
            }
            if (added) showInTree(currentFolder_);
            else status(L"请拖入存放图片的文件夹。");
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        if (comparing_) {
            const bool wasSecondary = s.secondary;
            back();
            if (wasSecondary) SetActiveWindow(main_.hwnd);
            return 0;
        }
        if (s.secondary) {
            closeSecond();
            return 0;
        }
        save();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (!s.secondary) {
            main_.hwnd = nullptr;
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(apartment)) return 1;
    int result{};
    {
        int count{};
        bool persist = true;
        std::vector<fs::path> folders;
        if (auto args = CommandLineToArgvW(GetCommandLineW(), &count)) {
            for (int i = 1; i < count; ++i) {
                if (wcscmp(args[i], L"--no-history") == 0) persist = false;
                else folders.emplace_back(args[i]);
            }
            LocalFree(args);
        }
        App app(instance, persist);
        if (!app.create(folders)) {
            MessageBoxW(nullptr, L"无法创建应用窗口。", L"PicCompare", MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            result = int(message.wParam);
        }
    }
    CoUninitialize();
    return result;
}
