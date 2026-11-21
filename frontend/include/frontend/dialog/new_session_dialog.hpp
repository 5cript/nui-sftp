#pragma once

#include <persistence/state/session_options.hpp>

#include <nui/frontend/element_renderer.hpp>
#include <nui/frontend/val.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class NewSessionDialog
{
  public:
    NewSessionDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(NewSessionDialog);

    Nui::ElementRenderer operator()();

    /**
     * @brief The server of a new FTP session as entered in the dialog.
     */
    struct FtpServer
    {
        std::string host{};
        /**
         * @brief None leaves the port to the TLS mode.
         */
        std::optional<int> port{};
        /**
         * @brief Empty to log in anonymously.
         */
        std::string userName{};
        Persistence::FtpTlsMode tlsMode{Persistence::FtpTlsMode::Explicit};
    };

    struct ConfirmResult
    {
        std::string sessionName{};
        std::string iconName{};
        Persistence::TerminalEngineType sessionType{Persistence::TerminalEngineType::ssh};
        /**
         * @brief Only filled for an FTP session.
         */
        FtpServer ftpServer{};
    };
    struct OpenOptions
    {
        std::function<void(ConfirmResult const&)> onConfirm;
        bool showIconPicker{true};
        bool showSessionTypePicker{true};
        std::string initialName{};
        std::string initialIcon{"laptop"};
        Persistence::TerminalEngineType initialSessionType{Persistence::TerminalEngineType::ssh};
    };
    void open(OpenOptions options);

  private:
    Nui::ElementRenderer dialogBody();

    /**
     * @brief The fields of an FTP server, shown while the FTP session type is picked.
     */
    Nui::ElementRenderer ftpServerFields();

    /**
     * @brief The ids of the fields shown, in the order Enter and the arrow keys move through them.
     */
    std::vector<std::string> visibleFieldIds() const;

    /**
     * @brief Focuses the field before or after the one with the id.
     *
     * @param direction -1 for the field before, +1 for the one after.
     * @return False when there is no such field.
     */
    bool focusNeighbour(std::string const& currentId, int direction) const;

    /**
     * @brief Closes the dialog as confirmed when the input is valid, else reports what is invalid.
     */
    void confirmIfValid();

    /**
     * @brief The keydown handler of a select: Enter moves to the next field or confirms after the last.
     */
    std::function<void(Nui::val)> selectKeyHandler(std::string fieldId);

    /**
     * @brief The keydown handler of a text field: Enter takes the typed value, then moves on or confirms. An invalid
     * value keeps the focus and shows what is wrong with it. The down arrow moves on in any case.
     *
     * @param syncValue Stores and checks the typed value, returns whether it is valid.
     */
    std::function<void(Nui::val)>
    textKeyHandler(std::string fieldId, std::function<bool(std::string const&)> syncValue);

    void checkInputValue(std::string const&);
    void checkFtpHostValue(std::string const& value);
    void checkFtpPortValue(std::string const& value);

    /**
     * @brief The id of the first field shown whose value is invalid: the name or, for an FTP session, a server field.
     */
    std::optional<std::string> firstInvalidFieldId() const;

    /**
     * @brief Whether the name and, for an FTP session, the server fields are valid.
     */
    bool inputIsValid() const;

    /**
     * @brief Focuses the first invalid field and shows what is wrong with it next to it.
     */
    void reportInvalidInput() const;

    /**
     * @brief The FTP server as entered, with the host and user name trimmed.
     */
    FtpServer ftpServer() const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};