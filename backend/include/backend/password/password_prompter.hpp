#include <backend/password/password_provider.hpp>
#include <nui/rpc.hpp>

class PasswordPrompter : public PasswordProvider
{
  public:
    PasswordPrompter(Nui::RpcHub& rpcHub);

    void registerRpc();

    void getPassword(
        std::string const& whatFor,
        std::string const& prompt,
        std::function<void(std::optional<std::string>)> const& onPasswordReady
    ) override;

  private:
    Nui::RpcHub* rpcHub_;
    std::function<void(std::optional<std::string>)> ready_;
};