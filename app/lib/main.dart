import 'package:material_ui/material_ui.dart';

import 'src/controller.dart';
import 'src/home_page.dart';
import 'src/rust/frb_generated.dart';

Future<void> main() async {
  await RustLib.init();
  runApp(const AirLinkApp());
}

class AirLinkApp extends StatefulWidget {
  const AirLinkApp({super.key});

  @override
  State<AirLinkApp> createState() => _AirLinkAppState();
}

class _AirLinkAppState extends State<AirLinkApp> {
  final controller = AirLinkController();

  @override
  void initState() {
    super.initState();
    controller.start();
  }

  @override
  void dispose() {
    controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    const seed = Color(0xFF2563EB);
    return MaterialApp(
      title: 'AirLink',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: seed),
        useMaterial3: true,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: seed,
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: HomePage(controller: controller),
    );
  }
}
