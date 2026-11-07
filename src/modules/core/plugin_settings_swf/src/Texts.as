package
{
   // The game translates $ON, $OFF, $BACK and $ACCEPT itself. These two strings have no game text.
   public final class Texts
   {

      private static const TABLE:Object = {
         "entry":{"en":"PLUGINS SETTINGS","pl":"USTAWIENIA WTYCZEK","de":"PLUGIN-EINSTELLUNGEN","fr":"PARAMÈTRES DES PLUGINS","it":"IMPOSTAZIONI PLUGIN","es":"AJUSTES DE PLUGINS","ru":"НАСТРОЙКИ ПЛАГИНОВ","zh":"插件设置","zh2":"外掛設定"},
         "no_plugins":{"en":"No xScal plugins","pl":"Brak wtyczek xScal","de":"Keine xScal-Plugins","fr":"Aucun plugin xScal","it":"Nessun plugin xScal","es":"Sin plugins de xScal","ru":"Нет плагинов xScal","zh":"没有 xScal 插件","zh2":"沒有 xScal 外掛"}
      };

      public static var language:String = "en";

      public static function get(key:String) : String
      {
         var texts:Object = TABLE[key];
         return texts[language] != null ? String(texts[language]) : String(texts["en"]);
      }
   }
}
