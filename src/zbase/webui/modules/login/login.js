ZBase.registerView((function() {

  var kServerErrorMsg = "Server Error; please try again and contact support if the problem persists.";

  var finishLogin = function() {
    ZBase.showLoader();
    ZBase.util.httpGet("/a/_/c", function(http) {
      ZBase.hideLoader();
      if (http.status != 200) {
        showErrorMessage(kServerErrorMsg);
        return;
      }

      ZBase.updateConfig(JSON.parse(http.responseText));
      ZBase.navigateTo("/a/");
    });
  };

  var tryLogin = function(authdata) {
    var postdata = ZBase.util.buildQueryString(authdata);

    ZBase.showLoader();
    ZBase.util.httpPost("/analytics/api/v1/auth/login", postdata, function(http) {
      ZBase.hideLoader();
      hideErrorMessage();

      if (http.status == 200) {
        finishLogin();
        return;
      }

      if (http.status == 401) {
        displayUserPrompt();
        showErrorMessage("Invalid Credentials");
        return;
      }

      showErrorMessage(kServerErrorMsg);
    });
  };

  var hideErrorMessage = function() {
    var viewport = document.getElementById("zbase_viewport");
    var elem = viewport.querySelector(".zbase_login .error_message");
    elem.classList.add("hidden");
  };

  var showErrorMessage = function(msg) {
    var viewport = document.getElementById("zbase_viewport");
    var elem = viewport.querySelector(".zbase_login .error_message");
    elem.innerHTML = msg;
    elem.classList.remove("hidden");
  };

  var displayUserPrompt = function() {
    var viewport = document.getElementById("zbase_viewport");
    var page = ZBase.getTemplate("login", "zbase_login_user_prompt_tpl");

    viewport.innerHTML = "";
    viewport.appendChild(page);

    var form = viewport.querySelector("form");
    form.addEventListener("submit", function(e) {
      e.preventDefault();

      tryLogin({
        userid: this.querySelector("input[name='userid']").value,
        password: this.querySelector("input[name='password']").value
      });

      return false;
    });
  }

  var displayNamespacePrompt = function(authdata) {
    var viewport = document.getElementById("zbase_viewport");
    var page = ZBase.getTemplate("login", "zbase_login_namespace_prompt_tpl");

    viewport.innerHTML = "";
    viewport.appendChild(page);
  }

  var render = function(path) {
    var conf = ZBase.getConfig();
    if (conf.current_user) {
      ZBase.navigateTo("/a/");
      return;
    }

    displayUserPrompt();
  };

  return {
    name: "login",
    loadView: function(params) { render(params.path); },
    unloadView: function() {},
    handleNavigationChange: render
  };
})());
