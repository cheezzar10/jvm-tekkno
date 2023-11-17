// javac -d bin *.java
// java -cp bin -agentpath:bin/agent.dylib Main
import static java.lang.System.out;

public class Main {
    public static void main(String[] args) throws Exception {
        Service service = new Service();

        for (int i = 0;i < 30;i++) {
            Thread.sleep(1000);

            int result = service.get();
            out.printf("result: %d%n", result);
        }
        
        out.println("exiting...");
    }
}