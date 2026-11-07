fragments:
fragments // {
  enable = true;
  doNotBuild = [ fragments.test.shadow.package ];
  test = fragments.test // {
    shadow = fragments.test.shadow // { enable = false; };
    qualifying = fragments.test.qualifying // { enable = true; branches = ":any"; };
    reenabled = fragments.test.reenabled // { enable = true; };
  };
}
