/*
 * Copyright (c) 2021-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TreeMapWidget.h"
#include <AK/Error.h>
#include <AK/LexicalPath.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/URL.h>
#include <Applications/SpaceAnalyzer/SpaceAnalyzerGML.h>
#include <LibCore/DirIterator.h>
#include <LibCore/File.h>
#include <LibCore/IODevice.h>
#include <LibCore/Stream.h>
#include <LibDesktop/Launcher.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Breadcrumbbar.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/FileIconProvider.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/Menubar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Statusbar.h>
#include <LibGfx/Bitmap.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr auto APP_NAME = "Space Analyzer"sv;
static constexpr size_t FILES_ENCOUNTERED_UPDATE_STEP_SIZE = 25;

struct TreeNode : public SpaceAnalyzer::TreeMapNode {
    TreeNode(DeprecatedString name)
        : m_name(move(name)) {};

    virtual DeprecatedString name() const override { return m_name; }
    virtual i64 area() const override { return m_area; }
    virtual size_t num_children() const override
    {
        if (m_children) {
            return m_children->size();
        }
        return 0;
    }
    virtual TreeNode const& child_at(size_t i) const override { return m_children->at(i); }
    virtual void sort_children_by_area() const override
    {
        if (m_children) {
            Vector<TreeNode>* children = const_cast<Vector<TreeNode>*>(m_children.ptr());
            quick_sort(*children, [](auto& a, auto& b) { return b.m_area < a.m_area; });
        }
    }

    DeprecatedString m_name;
    i64 m_area { 0 };
    OwnPtr<Vector<TreeNode>> m_children;
};

struct Tree : public SpaceAnalyzer::TreeMap {
    Tree(DeprecatedString root_name)
        : m_root(move(root_name)) {};
    virtual ~Tree() {};
    TreeNode m_root;
    virtual SpaceAnalyzer::TreeMapNode const& root() const override
    {
        return m_root;
    };
};

struct MountInfo {
    DeprecatedString mount_point;
    DeprecatedString source;
};

static ErrorOr<void> fill_mounts(Vector<MountInfo>& output)
{
    // Output info about currently mounted filesystems.
    auto file = TRY(Core::Stream::File::open("/sys/kernel/df"sv, Core::Stream::OpenMode::Read));

    auto content = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(content));

    TRY(json.as_array().try_for_each([&output](JsonValue const& value) -> ErrorOr<void> {
        auto& filesystem_object = value.as_object();
        MountInfo mount_info;
        mount_info.mount_point = filesystem_object.get("mount_point"sv).to_deprecated_string();
        mount_info.source = filesystem_object.get("source"sv).as_string_or("none"sv);
        TRY(output.try_append(mount_info));
        return {};
    }));

    return {};
}

static MountInfo* find_mount_for_path(DeprecatedString path, Vector<MountInfo>& mounts)
{
    MountInfo* result = nullptr;
    size_t length = 0;
    for (auto& mount_info : mounts) {
        DeprecatedString& mount_point = mount_info.mount_point;
        if (path.starts_with(mount_point)) {
            if (!result || mount_point.length() > length) {
                result = &mount_info;
                length = mount_point.length();
            }
        }
    }
    return result;
}

static long long int update_totals(TreeNode& node)
{
    long long int result = 0;
    if (node.m_children) {
        for (auto& child : *node.m_children) {
            result += update_totals(child);
        }
        node.m_area = result;
    } else {
        result = node.m_area;
    }
    return result;
}

static NonnullRefPtr<GUI::Window> create_progress_window()
{
    auto window = GUI::Window::construct();

    window->set_title(APP_NAME);
    window->set_resizable(false);
    window->set_closeable(false);
    window->resize(240, 50);
    window->center_on_screen();

    auto main_widget = window->set_main_widget<GUI::Widget>().release_value_but_fixme_should_propagate_errors();
    main_widget->set_fill_with_background_color(true);
    main_widget->set_layout<GUI::VerticalBoxLayout>();

    auto& label = main_widget->add<GUI::Label>("Analyzing storage space...");
    label.set_fixed_height(22);

    auto& progresslabel = main_widget->add<GUI::Label>();
    progresslabel.set_name("progresslabel");
    progresslabel.set_fixed_height(22);

    return window;
}

static void update_progress_label(GUI::Label& progresslabel, size_t files_encountered_count)
{
    auto text = DeprecatedString::formatted("{} files...", files_encountered_count);
    progresslabel.set_text(text);

    Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
}

struct QueueEntry {
    QueueEntry(DeprecatedString path, TreeNode* node)
        : path(move(path))
        , node(node) {};
    DeprecatedString path;
    TreeNode* node { nullptr };
};

static void populate_filesize_tree(TreeNode& root, Vector<MountInfo>& mounts, HashMap<int, int>& error_accumulator, GUI::Label& progresslabel)
{
    VERIFY(!root.m_name.ends_with('/'));

    Queue<QueueEntry> queue;
    queue.enqueue(QueueEntry(root.m_name, &root));
    size_t files_encountered_count = 0;

    StringBuilder builder = StringBuilder();
    builder.append(root.m_name);
    builder.append('/');
    MountInfo* root_mount_info = find_mount_for_path(builder.to_deprecated_string(), mounts);
    if (!root_mount_info) {
        return;
    }
    while (!queue.is_empty()) {
        QueueEntry queue_entry = queue.dequeue();

        builder.clear();
        builder.append(queue_entry.path);
        builder.append('/');

        MountInfo* mount_info = find_mount_for_path(builder.to_deprecated_string(), mounts);
        if (!mount_info || (mount_info != root_mount_info && mount_info->source != root_mount_info->source)) {
            continue;
        }

        Core::DirIterator dir_iterator(builder.to_deprecated_string(), Core::DirIterator::SkipParentAndBaseDir);
        if (dir_iterator.has_error()) {
            int error_sum = error_accumulator.get(dir_iterator.error()).value_or(0);
            error_accumulator.set(dir_iterator.error(), error_sum + 1);
        } else {
            queue_entry.node->m_children = make<Vector<TreeNode>>();
            while (dir_iterator.has_next()) {
                queue_entry.node->m_children->append(TreeNode(dir_iterator.next_path()));
            }
            for (auto& child : *queue_entry.node->m_children) {
                files_encountered_count += 1;
                if (!(files_encountered_count % FILES_ENCOUNTERED_UPDATE_STEP_SIZE))
                    update_progress_label(progresslabel, files_encountered_count);

                DeprecatedString& name = child.m_name;
                int name_len = name.length();
                builder.append(name);
                struct stat st;
                int stat_result = fstatat(dir_iterator.fd(), name.characters(), &st, AT_SYMLINK_NOFOLLOW);
                if (stat_result < 0) {
                    int error_sum = error_accumulator.get(errno).value_or(0);
                    error_accumulator.set(errno, error_sum + 1);
                } else {
                    if (S_ISDIR(st.st_mode)) {
                        queue.enqueue(QueueEntry(builder.to_deprecated_string(), &child));
                    } else {
                        child.m_area = st.st_size;
                    }
                }
                builder.trim(name_len);
            }
        }
    }

    update_totals(root);
}

static ErrorOr<void> analyze(RefPtr<Tree> tree, SpaceAnalyzer::TreeMapWidget& treemapwidget, GUI::Statusbar& statusbar)
{
    statusbar.set_text("");
    auto progress_window = create_progress_window();
    progress_window->show();

    auto& progresslabel = *progress_window->main_widget()->find_descendant_of_type_named<GUI::Label>("progresslabel");
    update_progress_label(progresslabel, 0);

    // Build an in-memory tree mirroring the filesystem and for each node
    // calculate the sum of the file size for all its descendants.
    TreeNode* root = &tree->m_root;
    Vector<MountInfo> mounts;
    TRY(fill_mounts(mounts));
    HashMap<int, int> error_accumulator;
    populate_filesize_tree(*root, mounts, error_accumulator, progresslabel);

    progress_window->close();

    // Display an error summary in the statusbar.
    if (!error_accumulator.is_empty()) {
        StringBuilder builder;
        bool first = true;
        builder.append("Some directories were not analyzed: "sv);
        for (auto& key : error_accumulator.keys()) {
            if (!first) {
                builder.append(", "sv);
            }
            auto const* error = strerror(key);
            builder.append({ error, strlen(error) });
            builder.append(" ("sv);
            int value = error_accumulator.get(key).value();
            builder.append(DeprecatedString::number(value));
            if (value == 1) {
                builder.append(" time"sv);
            } else {
                builder.append(" times"sv);
            }
            builder.append(')');
            first = false;
        }
        statusbar.set_text(builder.to_deprecated_string());
    } else {
        statusbar.set_text("No errors");
    }
    treemapwidget.set_tree(tree);

    return {};
}

static bool is_removable(DeprecatedString const& absolute_path)
{
    VERIFY(!absolute_path.is_empty());
    int access_result = access(LexicalPath::dirname(absolute_path).characters(), W_OK);
    if (access_result != 0 && errno != EACCES)
        perror("access");
    return access_result == 0;
}

static DeprecatedString get_absolute_path_to_selected_node(SpaceAnalyzer::TreeMapWidget const& treemapwidget, bool include_last_node = true)
{
    StringBuilder path_builder;
    for (size_t k = 0; k < treemapwidget.path_size() - (include_last_node ? 0 : 1); k++) {
        if (k != 0) {
            path_builder.append('/');
        }
        SpaceAnalyzer::TreeMapNode const* node = treemapwidget.path_node(k);
        path_builder.append(node->name());
    }
    return path_builder.build();
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::try_create(arguments));

    RefPtr<Tree> tree = adopt_ref(*new Tree(""));

    // Configure application window.
    auto app_icon = GUI::Icon::default_icon("app-space-analyzer"sv);
    auto window = GUI::Window::construct();
    window->set_title(APP_NAME);
    window->resize(640, 480);
    window->set_icon(app_icon.bitmap_for_size(16));

    // Load widgets.
    auto mainwidget = TRY(window->set_main_widget<GUI::Widget>());
    TRY(mainwidget->load_from_gml(space_analyzer_gml));
    auto& breadcrumbbar = *mainwidget->find_descendant_of_type_named<GUI::Breadcrumbbar>("breadcrumbbar");
    auto& treemapwidget = *mainwidget->find_descendant_of_type_named<SpaceAnalyzer::TreeMapWidget>("tree_map");
    auto& statusbar = *mainwidget->find_descendant_of_type_named<GUI::Statusbar>("statusbar");

    treemapwidget.set_focus(true);

    auto& file_menu = window->add_menu("&File");
    file_menu.add_action(GUI::Action::create("&Analyze", [&](auto&) {
        if (auto result = analyze(tree, treemapwidget, statusbar); result.is_error()) {
            GUI::MessageBox::show_error(window, DeprecatedString::formatted("{}", result.error()));
        }
    }));
    file_menu.add_separator();
    file_menu.add_action(GUI::CommonActions::make_quit_action([&](auto&) {
        app->quit();
    }));

    auto& help_menu = window->add_menu("&Help");
    help_menu.add_action(GUI::CommonActions::make_command_palette_action(window));
    help_menu.add_action(GUI::CommonActions::make_about_action(APP_NAME, app_icon, window));

    auto open_icon = TRY(Gfx::Bitmap::try_load_from_file("/res/icons/16x16/open.png"sv));
    // Configure the nodes context menu.
    auto open_folder_action = GUI::Action::create("Open Folder", { Mod_Ctrl, Key_O }, open_icon, [&](auto&) {
        Desktop::Launcher::open(URL::create_with_file_scheme(get_absolute_path_to_selected_node(treemapwidget)));
    });
    auto open_containing_folder_action = GUI::Action::create("Open Containing Folder", { Mod_Ctrl, Key_O }, open_icon, [&](auto&) {
        LexicalPath path { get_absolute_path_to_selected_node(treemapwidget) };
        Desktop::Launcher::open(URL::create_with_file_scheme(path.dirname(), path.basename()));
    });

    auto copy_icon = TRY(Gfx::Bitmap::try_load_from_file("/res/icons/16x16/edit-copy.png"sv));
    auto copy_path_action = GUI::Action::create("Copy Path to Clipboard", { Mod_Ctrl, Key_C }, copy_icon, [&](auto&) {
        GUI::Clipboard::the().set_plain_text(get_absolute_path_to_selected_node(treemapwidget));
    });
    auto delete_action = GUI::CommonActions::make_delete_action([&](auto&) {
        DeprecatedString selected_node_path = get_absolute_path_to_selected_node(treemapwidget);
        bool try_again = true;
        while (try_again) {
            try_again = false;

            auto deletion_result = Core::File::remove(selected_node_path, Core::File::RecursionMode::Allowed);
            if (deletion_result.is_error()) {
                auto retry_message_result = GUI::MessageBox::show(window,
                    DeprecatedString::formatted("Failed to delete \"{}\": {}. Retry?",
                        selected_node_path,
                        deletion_result.error()),
                    "Deletion failed"sv,
                    GUI::MessageBox::Type::Error,
                    GUI::MessageBox::InputType::YesNo);
                if (retry_message_result == GUI::MessageBox::ExecResult::Yes) {
                    try_again = true;
                }
            } else {
                GUI::MessageBox::show(window,
                    DeprecatedString::formatted("Successfully deleted \"{}\".", selected_node_path),
                    "Deletion completed"sv,
                    GUI::MessageBox::Type::Information,
                    GUI::MessageBox::InputType::OK);
            }
        }

        // TODO: Refreshing data always causes resetting the viewport back to "/".
        // It would be great if we found a way to preserve viewport across refreshes.
        if (auto result = analyze(tree, treemapwidget, statusbar); result.is_error()) {
            GUI::MessageBox::show_error(window, DeprecatedString::formatted("{}", result.error()));
        }
    });

    auto context_menu = GUI::Menu::construct();
    context_menu->add_action(*open_folder_action);
    context_menu->add_action(*open_containing_folder_action);
    context_menu->add_action(*copy_path_action);
    context_menu->add_action(*delete_action);

    // Configure event handlers.
    breadcrumbbar.on_segment_click = [&](size_t index) {
        VERIFY(index < treemapwidget.path_size());
        treemapwidget.set_viewpoint(index);
    };
    treemapwidget.on_path_change = [&]() {
        StringBuilder builder;

        breadcrumbbar.clear_segments();
        for (size_t k = 0; k < treemapwidget.path_size(); k++) {
            if (k == 0) {
                if (treemapwidget.viewpoint() == 0)
                    window->set_title("/ - SpaceAnalyzer");

                breadcrumbbar.append_segment("/", GUI::FileIconProvider::icon_for_path("/").bitmap_for_size(16), "/", "/");
                continue;
            }

            const SpaceAnalyzer::TreeMapNode* node = treemapwidget.path_node(k);

            builder.append('/');
            builder.append(node->name());

            // Sneakily set the window title here, while the StringBuilder holds the right amount of the path.
            if (k == treemapwidget.viewpoint())
                window->set_title(DeprecatedString::formatted("{} - SpaceAnalyzer", builder.string_view()));

            breadcrumbbar.append_segment(node->name(), GUI::FileIconProvider::icon_for_path(builder.string_view()).bitmap_for_size(16), builder.string_view(), builder.string_view());
        }
        breadcrumbbar.set_selected_segment(treemapwidget.viewpoint());
    };
    treemapwidget.on_context_menu_request = [&](const GUI::ContextMenuEvent& event) {
        DeprecatedString selected_node_path = get_absolute_path_to_selected_node(treemapwidget);
        if (selected_node_path.is_empty())
            return;
        delete_action->set_enabled(is_removable(selected_node_path));
        if (Core::File::is_directory(selected_node_path)) {
            open_folder_action->set_visible(true);
            open_containing_folder_action->set_visible(false);
        } else {
            open_folder_action->set_visible(false);
            open_containing_folder_action->set_visible(true);
        }
        context_menu->popup(event.screen_position());
    };

    // At startup automatically do an analysis of root.
    TRY(analyze(tree, treemapwidget, statusbar));

    window->show();
    return app->exec();
}
