using System.ComponentModel;
using System.Windows;

namespace AudioBridgeUI;

/// <summary>
/// Code-behind for MainWindow. Minimal logic -- the DataContext is set
/// externally by App.xaml.cs and all behavior lives in the view model.
/// </summary>
public partial class MainWindow : Window
{
    /// <summary>
    /// When true, the window will actually close instead of hiding to the tray.
    /// Set by the tray "Exit" handler before application shutdown.
    /// </summary>
    public bool AllowClose { get; set; }

    public MainWindow()
    {
        InitializeComponent();
    }

    /// <summary>
    /// Intercepts the window close and hides to the system tray instead.
    /// The application exits only through the tray context menu "Exit" item,
    /// which sets <see cref="AllowClose"/> to bypass the hide behavior.
    /// </summary>
    protected override void OnClosing(CancelEventArgs e)
    {
        if (!AllowClose)
        {
            e.Cancel = true;
            Hide();
        }
    }
}
