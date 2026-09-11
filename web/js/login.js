(function () {
  "use strict";

  var form = document.getElementById("loginForm");
  var errorBox = document.getElementById("loginError");
  var submit = document.getElementById("loginSubmit");

  function showError(message) {
    errorBox.className = "d-none";
    if (window.HsfAlert) window.HsfAlert(message, "Sign in failed", "error");
  }

  function clearError() {
    errorBox.textContent = "";
    errorBox.className = "d-none";
  }

  form.addEventListener("submit", function (event) {
    event.preventDefault();
    clearError();

    var username = document.getElementById("username").value;
    var password = document.getElementById("password").value;

    // Disabled while in flight: harmless against the current local check,
    // but this is the seam that becomes a network round trip, and a
    // double-submit there would fire two logins.
    submit.disabled = true;
    submit.textContent = "Signing in...";

    window.HsfAuth.login(username, password)
      .then(function (result) {
        if (!result.ok) {
          showError(result.error || "Sign in failed");
          document.getElementById("password").value = "";
          document.getElementById("password").focus();
          return;
        }

        // Only same-origin paths -- taking ?next verbatim would let a
        // crafted link bounce someone off-site straight after login.
        var params = new URLSearchParams(window.location.search);
        var next = params.get("next");
        var safe = next && next.charAt(0) === "/" && next.charAt(1) !== "/" ? next : "/index.html";
        window.location.replace(safe);
      })
      .catch(function (err) {
        showError("Sign in failed: " + (err && err.message ? err.message : "unknown error"));
      })
      .finally(function () {
        submit.disabled = false;
        submit.textContent = "Sign in";
      });
  });
})();
