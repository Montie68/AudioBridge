using System.Windows.Input;

namespace AudioBridgeUI.ViewModels;

/// <summary>
/// A simple <see cref="ICommand"/> implementation that delegates to Action/Func callbacks.
/// Supports both synchronous and async execute delegates.
/// </summary>
public class RelayCommand : ICommand
{
    private readonly Action<object?> _execute;
    private readonly Func<object?, bool>? _canExecute;

    public event EventHandler? CanExecuteChanged
    {
        add => CommandManager.RequerySuggested += value;
        remove => CommandManager.RequerySuggested -= value;
    }

    /// <summary>
    /// Creates a RelayCommand with an execute action and an optional canExecute predicate.
    /// </summary>
    public RelayCommand(Action<object?> execute, Func<object?, bool>? canExecute = null)
    {
        _execute = execute ?? throw new ArgumentNullException(nameof(execute));
        _canExecute = canExecute;
    }

    /// <summary>
    /// Convenience constructor for parameterless actions.
    /// </summary>
    public RelayCommand(Action execute, Func<bool>? canExecute = null)
        : this(_ => execute(), canExecute is not null ? _ => canExecute() : null)
    {
    }

    public bool CanExecute(object? parameter) =>
        _canExecute?.Invoke(parameter) ?? true;

    public void Execute(object? parameter) =>
        _execute(parameter);

    /// <summary>
    /// Forces WPF to re-evaluate CanExecute for all commands.
    /// </summary>
    public static void RaiseCanExecuteChanged() =>
        CommandManager.InvalidateRequerySuggested();
}

/// <summary>
/// An async-aware RelayCommand that wraps an async delegate.
/// Prevents concurrent execution by default.
/// </summary>
public class AsyncRelayCommand : ICommand
{
    private readonly Func<object?, Task> _execute;
    private readonly Func<object?, bool>? _canExecute;
    private bool _isExecuting;

    public event EventHandler? CanExecuteChanged
    {
        add => CommandManager.RequerySuggested += value;
        remove => CommandManager.RequerySuggested -= value;
    }

    public AsyncRelayCommand(Func<object?, Task> execute, Func<object?, bool>? canExecute = null)
    {
        _execute = execute ?? throw new ArgumentNullException(nameof(execute));
        _canExecute = canExecute;
    }

    /// <summary>
    /// Convenience constructor for parameterless async actions.
    /// </summary>
    public AsyncRelayCommand(Func<Task> execute, Func<bool>? canExecute = null)
        : this(_ => execute(), canExecute is not null ? _ => canExecute() : null)
    {
    }

    public bool CanExecute(object? parameter) =>
        !_isExecuting && (_canExecute?.Invoke(parameter) ?? true);

    public async void Execute(object? parameter)
    {
        if (_isExecuting)
            return;

        _isExecuting = true;
        CommandManager.InvalidateRequerySuggested();

        try
        {
            await _execute(parameter);
        }
        finally
        {
            _isExecuting = false;
            CommandManager.InvalidateRequerySuggested();
        }
    }
}
